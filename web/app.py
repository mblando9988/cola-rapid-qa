import asyncio
import json
import os
import re
import shutil
import subprocess
import time
from contextlib import asynccontextmanager, suppress
from pathlib import Path
from uuid import uuid4

from fastapi import FastAPI, File, UploadFile, HTTPException, Form
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
import uvicorn

from . import rapid_ocr

BASE_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = BASE_DIR.parent
BINARY_PATH = Path(os.environ.get(
    "COLA_ANALYZER_PATH", str(PROJECT_ROOT / "bin" / "cola_label_qa")
))
# Sample PDFs: env var override, otherwise the repository-level samples directory.
SAMPLE_DIR = Path(os.environ.get("SAMPLE_DIR", str(PROJECT_ROOT / "samples")))
DEMO_SAMPLE_NAME = os.environ.get("DEMO_SAMPLE_NAME", "demo-cola.pdf")
RUNS_DIR = Path(os.environ.get("RUNS_DIR", str(PROJECT_ROOT / "web_runs")))
RUNS_DIR.mkdir(exist_ok=True)
SAMPLE_DIR.mkdir(exist_ok=True)

MAX_UPLOAD_BYTES = int(os.environ.get("MAX_UPLOAD_BYTES", 50 * 1024 * 1024))
MAX_CONCURRENT_ANALYSES = int(os.environ.get("MAX_CONCURRENT_ANALYSES", 1))
RUN_RETENTION_HOURS = float(os.environ.get("RUN_RETENTION_HOURS", 24))
CLEANUP_INTERVAL_SECONDS = int(os.environ.get("CLEANUP_INTERVAL_SECONDS", 3600))

if min(MAX_UPLOAD_BYTES, MAX_CONCURRENT_ANALYSES, CLEANUP_INTERVAL_SECONDS) <= 0:
    raise RuntimeError("Upload, concurrency, and cleanup limits must be positive")
if RUN_RETENTION_HOURS <= 0:
    raise RuntimeError("RUN_RETENTION_HOURS must be positive")

ANALYSIS_SEMAPHORE = asyncio.Semaphore(MAX_CONCURRENT_ANALYSES)
RUN_ID_RE = re.compile(r"^(?:run|sample)_[0-9a-f]{32}$")
IMAGE_NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+\.(?:png|jpe?g)$", re.IGNORECASE)


def cleanup_old_runs(max_age_hours: float = RUN_RETENTION_HOURS) -> int:
    """Delete expired application-owned run directories."""
    cutoff = time.time() - max_age_hours * 3600
    removed = 0
    for entry in RUNS_DIR.iterdir():
        if not entry.is_dir() or not RUN_ID_RE.fullmatch(entry.name):
            continue
        try:
            if entry.stat().st_mtime < cutoff:
                shutil.rmtree(entry)
                removed += 1
        except OSError as exc:
            print(f"run cleanup failed for {entry}: {exc}", flush=True)
    return removed


def _warm_engine():
    """Load RapidOCR before the server reports ready."""
    rapid_ocr.get_engine()
    print("rapid_ocr engine ready", flush=True)


async def _periodic_cleanup() -> None:
    while True:
        await asyncio.sleep(CLEANUP_INTERVAL_SECONDS)
        await asyncio.to_thread(cleanup_old_runs)


@asynccontextmanager
async def app_lifespan(_app: FastAPI):
    await asyncio.to_thread(cleanup_old_runs)
    await asyncio.to_thread(_warm_engine)
    cleanup_task = asyncio.create_task(_periodic_cleanup())
    try:
        yield
    finally:
        cleanup_task.cancel()
        with suppress(asyncio.CancelledError):
            await cleanup_task


app = FastAPI(
    title="COLA PDF Parser & Label QA System",
    version="2.1.0",
    lifespan=app_lifespan,
)


def _validate_pdf_name(filename: str | None) -> str:
    if not filename or filename in {".", ".."}:
        raise HTTPException(status_code=400, detail="A PDF filename is required.")
    if "/" in filename or "\\" in filename or Path(filename).name != filename:
        raise HTTPException(status_code=400, detail="PDF filename must not contain a path.")
    if not filename.lower().endswith(".pdf"):
        raise HTTPException(status_code=400, detail="Uploaded file must be a PDF.")
    return filename


def _new_run_dir(prefix: str) -> tuple[str, Path]:
    run_id = f"{prefix}_{uuid4().hex}"
    run_dir = RUNS_DIR / run_id
    run_dir.mkdir(mode=0o700)
    return run_id, run_dir


async def _save_upload_pdf(file: UploadFile, destination: Path) -> None:
    total = 0
    header = bytearray()
    try:
        with destination.open("xb") as output:
            while chunk := await file.read(1024 * 1024):
                total += len(chunk)
                if total > MAX_UPLOAD_BYTES:
                    raise HTTPException(status_code=413, detail="PDF exceeds the server upload limit.")
                if len(header) < 1024:
                    header.extend(chunk[: 1024 - len(header)])
                output.write(chunk)
        if b"%PDF-" not in header:
            raise HTTPException(status_code=400, detail="Uploaded content is not a PDF.")
    except Exception:
        destination.unlink(missing_ok=True)
        raise
    finally:
        await file.close()


def run_cola_analyzer(pdf_path: Path, output_dir: Path) -> dict:
    """Stage 1 (C++: MuPDF form + image extraction) then RapidOCR + match."""
    if not BINARY_PATH.is_file() or not os.access(BINARY_PATH, os.X_OK):
        raise HTTPException(
            status_code=503,
            detail="C++ analyzer is unavailable."
        )

    output_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(BINARY_PATH),
        str(pdf_path),
        "--task", "form",
        "--out", str(output_dir),
        "--json"
    ]

    try:
        proc = subprocess.run(
            cmd,
            cwd=str(PROJECT_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=60
        )
    except subprocess.TimeoutExpired:
        raise HTTPException(status_code=504, detail="Form extraction timed out (60s limit).")

    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "unknown analyzer failure")[-2000:]
        raise HTTPException(status_code=500, detail=f"Form extraction failed: {detail}")

    # The C++ binary writes cola_analysis.json next to the PDF (the run dir).
    json_path = output_dir.parent / "cola_analysis.json"
    if not json_path.exists():
        json_path = output_dir / "cola_analysis.json"

    if not json_path.exists():
        raise HTTPException(
            status_code=500,
            detail=f"Form extraction produced no JSON. Output: {proc.stdout} Error: {proc.stderr}"
        )

    try:
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        raise HTTPException(status_code=500, detail=f"Invalid analyzer JSON: {exc}") from exc

    # Stage 2: RapidOCR over the extracted images + the C++ matching logic.
    try:
        ocr_result = rapid_ocr.ocr_and_match(data)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"OCR failed: {e}")

    data["task"] = "both"
    data["ocr"] = ocr_result["ocr"]
    data["matches"] = ocr_result["matches"]
    data["class_type"] = ocr_result["class_type"]

    # Add web-accessible URLs for native MuPDF page renders and extracted images.
    # We intentionally do NOT inline base64: it bloats the response (~1.7 MB -> ~19 KB)
    # and delays the verdict render; the browser fetches these in parallel instead.
    for collection in ("document_pages", "images"):
        if collection not in data or not isinstance(data[collection], list):
            continue
        for img in data[collection]:
            img_name = img.get("name")
            if img_name and IMAGE_NAME_RE.fullmatch(img_name):
                img_path = output_dir / img_name
                if img_path.exists():
                    img["url"] = f"/runs/{output_dir.parent.name}/images/{img_name}"

    return data


@app.get("/api/health")
async def health_check():
    binary_ready = BINARY_PATH.is_file() and os.access(BINARY_PATH, os.X_OK)
    ocr_ready = rapid_ocr.engine_ready()
    ready = binary_ready and ocr_ready
    return JSONResponse(
        {
            "status": "healthy" if ready else "unhealthy",
            "binary_ready": binary_ready,
            "ocr_ready": ocr_ready,
            "ocr_backend": "rapidocr",
        },
        status_code=200 if ready else 503,
    )


@app.get("/api/samples")
async def list_samples():
    """List only the synthetic demo application."""
    samples = []
    demo = SAMPLE_DIR / DEMO_SAMPLE_NAME
    if demo.is_file():
        samples.append({
            "filename": demo.name,
            "size_kb": round(demo.stat().st_size / 1024, 1),
            "synthetic": True,
        })
    return {"samples": samples}


@app.get("/runs/{run_id}/images/{image_name}", response_class=FileResponse)
async def serve_run_image(run_id: str, image_name: str):
    if not RUN_ID_RE.fullmatch(run_id) or not IMAGE_NAME_RE.fullmatch(image_name):
        raise HTTPException(status_code=404, detail="Image not found.")
    image_dir = (RUNS_DIR / run_id / "images").resolve()
    image_path = (image_dir / image_name).resolve()
    if image_path.parent != image_dir or not image_path.is_file():
        raise HTTPException(status_code=404, detail="Image not found.")
    return FileResponse(image_path)


@app.post("/api/analyze")
async def analyze_uploaded_pdf(file: UploadFile = File(...)):
    """Upload and analyze a COLA PDF file."""
    filename = _validate_pdf_name(file.filename)
    run_id, run_dir = _new_run_dir("run")
    input_pdf_path = run_dir / filename
    try:
        await _save_upload_pdf(file, input_pdf_path)
        async with ANALYSIS_SEMAPHORE:
            result = await asyncio.to_thread(run_cola_analyzer, input_pdf_path, run_dir / "images")
    except Exception:
        shutil.rmtree(run_dir, ignore_errors=True)
        raise
    result["run_id"] = run_id
    result["filename"] = filename
    return result


@app.post("/api/analyze-sample")
async def analyze_sample(filename: str = Form(...)):
    """Analyze the pre-configured synthetic demo PDF."""
    filename = _validate_pdf_name(filename)
    if filename != DEMO_SAMPLE_NAME:
        raise HTTPException(status_code=404, detail="Demo sample not found.")
    sample_root = SAMPLE_DIR.resolve()
    sample_path = (sample_root / filename).resolve()
    if sample_path.parent != sample_root or not sample_path.is_file():
        raise HTTPException(status_code=404, detail=f"Sample '{filename}' not found.")

    run_id, run_dir = _new_run_dir("sample")
    copied_pdf = run_dir / filename
    try:
        shutil.copyfile(sample_path, copied_pdf)
        async with ANALYSIS_SEMAPHORE:
            result = await asyncio.to_thread(run_cola_analyzer, copied_pdf, run_dir / "images")
    except Exception:
        shutil.rmtree(run_dir, ignore_errors=True)
        raise
    result["run_id"] = run_id
    result["filename"] = filename
    return result


@app.get("/", response_class=HTMLResponse)
async def serve_dashboard():
    index_file = BASE_DIR / "templates" / "index.html"
    if not index_file.exists():
        return HTMLResponse("<h1>Web UI template not found</h1>", status_code=500)
    with open(index_file, "r", encoding="utf-8") as f:
        return HTMLResponse(f.read())


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8080))
    print(f"Starting COLA QA Web Workflow on http://localhost:{port}")
    uvicorn.run("web.app:app", host="0.0.0.0", port=port, reload=False)
