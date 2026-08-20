import io
import os
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from fastapi import HTTPException, UploadFile

from web import app, rapid_ocr


class ReliabilityTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.sample_tempdir = tempfile.TemporaryDirectory()
        self.old_runs_dir = app.RUNS_DIR
        self.old_sample_dir = app.SAMPLE_DIR
        self.old_upload_limit = app.MAX_UPLOAD_BYTES
        app.RUNS_DIR = Path(self.tempdir.name)
        app.SAMPLE_DIR = Path(self.sample_tempdir.name)

    def tearDown(self):
        app.RUNS_DIR = self.old_runs_dir
        app.SAMPLE_DIR = self.old_sample_dir
        app.MAX_UPLOAD_BYTES = self.old_upload_limit
        self.sample_tempdir.cleanup()
        self.tempdir.cleanup()

    async def test_upload_rejects_path_and_creates_no_run(self):
        upload = UploadFile(filename="../escape.pdf", file=io.BytesIO(b"%PDF-1.7"))
        with self.assertRaises(HTTPException) as raised:
            await app.analyze_uploaded_pdf(upload)
        self.assertEqual(raised.exception.status_code, 400)
        self.assertEqual(list(app.RUNS_DIR.iterdir()), [])

    async def test_sample_rejects_path(self):
        with self.assertRaises(HTTPException) as raised:
            await app.analyze_sample("../sample-cola.pdf")
        self.assertEqual(raised.exception.status_code, 400)

    async def test_sample_api_exposes_only_synthetic_demo(self):
        (app.SAMPLE_DIR / app.DEMO_SAMPLE_NAME).write_bytes(b"%PDF-1.7\n")
        (app.SAMPLE_DIR / "real-application.pdf").write_bytes(b"%PDF-1.7\n")
        result = await app.list_samples()
        self.assertEqual(result["samples"], [{
            "filename": app.DEMO_SAMPLE_NAME,
            "size_kb": 0.0,
            "synthetic": True,
        }])

    async def test_sample_api_rejects_non_demo_pdf(self):
        with self.assertRaises(HTTPException) as raised:
            await app.analyze_sample("real-application.pdf")
        self.assertEqual(raised.exception.status_code, 404)

    async def test_upload_rejects_non_pdf_and_removes_partial_run(self):
        upload = UploadFile(filename="fake.pdf", file=io.BytesIO(b"plain text"))
        with self.assertRaises(HTTPException) as raised:
            await app.analyze_uploaded_pdf(upload)
        self.assertEqual(raised.exception.status_code, 400)
        self.assertEqual(list(app.RUNS_DIR.iterdir()), [])

    async def test_upload_limit_is_server_side(self):
        app.MAX_UPLOAD_BYTES = 8
        upload = UploadFile(filename="large.pdf", file=io.BytesIO(b"%PDF-1.7-extra"))
        with self.assertRaises(HTTPException) as raised:
            await app.analyze_uploaded_pdf(upload)
        self.assertEqual(raised.exception.status_code, 413)
        self.assertEqual(list(app.RUNS_DIR.iterdir()), [])

    async def test_health_is_unhealthy_without_binary(self):
        old_binary = app.BINARY_PATH
        old_engine = app.rapid_ocr._engine
        try:
            app.BINARY_PATH = app.RUNS_DIR / "missing"
            app.rapid_ocr._engine = object()
            response = await app.health_check()
        finally:
            app.BINARY_PATH = old_binary
            app.rapid_ocr._engine = old_engine
        self.assertEqual(response.status_code, 503)
        self.assertIn(b'"status":"unhealthy"', response.body)

    async def test_run_image_route_rejects_unowned_paths(self):
        with self.assertRaises(HTTPException) as raised:
            await app.serve_run_image("../audit", "result.json")
        self.assertEqual(raised.exception.status_code, 404)

    async def test_run_image_route_serves_native_document_page(self):
        run_id = f"run_{'b' * 32}"
        image_dir = app.RUNS_DIR / run_id / "images"
        image_dir.mkdir(parents=True)
        source = image_dir / "document_p1.png"
        source.write_bytes(b"\x89PNG\r\n\x1a\n")
        response = await app.serve_run_image(run_id, source.name)
        self.assertEqual(Path(response.path).resolve(), source.resolve())

    def test_cleanup_only_removes_owned_run_directories(self):
        owned = app.RUNS_DIR / f"run_{'a' * 32}"
        protected = app.RUNS_DIR / "readme_audit"
        owned.mkdir()
        protected.mkdir()
        old = time.time() - 7200
        os.utime(owned, (old, old))
        os.utime(protected, (old, old))
        self.assertEqual(app.cleanup_old_runs(max_age_hours=1), 1)
        self.assertFalse(owned.exists())
        self.assertTrue(protected.exists())

    def test_analyzer_nonzero_exit_cannot_use_stale_json(self):
        binary = app.RUNS_DIR / "analyzer"
        binary.write_text("#!/bin/sh\nexit 7\n", encoding="utf-8")
        binary.chmod(0o700)
        output = app.RUNS_DIR / "images"
        old_binary = app.BINARY_PATH
        app.BINARY_PATH = binary
        try:
            with patch("web.app.subprocess.run") as run:
                run.return_value.returncode = 7
                run.return_value.stderr = "failed"
                run.return_value.stdout = ""
                with self.assertRaises(HTTPException) as raised:
                    app.run_cola_analyzer(app.RUNS_DIR / "input.pdf", output)
        finally:
            app.BINARY_PATH = old_binary
        self.assertEqual(raised.exception.status_code, 500)

    def test_brand_match_preserves_literal_ocr_line(self):
        lines = [("p2_2.png", [{
            "text": "CARLOGIACOSA", "conf": 0.99,
            "x1": 1, "y1": 2, "x2": 30, "y2": 12,
        }])]
        result = rapid_ocr.match_answer(
            "Brand", "5. BRAND NAME (Required)", "CARLO GIACOSA",
            rapid_ocr.tagged_corpus(lines),
        )
        self.assertEqual(result["status"], "MATCH")
        self.assertEqual(result["evidence"]["image"], "p2_2.png")
        self.assertEqual(result["evidence"]["line"], "CARLOGIACOSA")

    def test_applicant_match_uses_full_used_on_label_name(self):
        lines = [("p3_1.png", [{
            "text": "LEVIGNOBLE,CORDOVA,TN", "conf": 0.95,
            "x1": 1, "y1": 2, "x2": 40, "y2": 12,
        }])]
        answer = (
            "LE VIGNOBLE, LLC 9369 ROCKY HILLS DR CORDOVA TN 38018 "
            "LE VIGNOBLE (Used on label)"
        )
        result = rapid_ocr.match_answer(
            "NameAddress", "7. NAME AND ADDRESS", answer,
            rapid_ocr.tagged_corpus(lines),
        )
        self.assertEqual(result["status"], "MATCH")
        self.assertEqual(result["evidence"]["line"], "LEVIGNOBLE,CORDOVA,TN")

    def test_selected_product_type_matches_literal_label_evidence(self):
        lines = [("p3_1.png", [{
            "text": "REDWINE-CONTAINSSULFITES", "conf": 0.99,
            "x1": 1, "y1": 2, "x2": 50, "y2": 12,
        }])]
        result = rapid_ocr.match_answer(
            "ProductType", "4. TYPE OF PRODUCT", "WINE",
            rapid_ocr.tagged_corpus(lines),
        )
        self.assertEqual(result["status"], "MATCH")
        self.assertEqual(result["evidence"]["line"], "REDWINE-CONTAINSSULFITES")

    def test_form_bbox_is_preserved_on_match_result(self):
        bbox = {"x1": 10, "y1": 20, "x2": 30, "y2": 40}
        form_data = {
            "images": [{"name": "p2_1.png", "path": "unused.png"}],
            "form_qa": [{
                "num": "5", "question": "5. BRAND NAME (Required)",
                "answer": "PRINSI", "bbox": bbox,
            }],
            "raw_text": [],
        }
        line = {
            "text": "Prinsi", "conf": 0.99,
            "x1": 1, "y1": 2, "x2": 30, "y2": 12,
        }
        with patch("web.rapid_ocr.ocr_lines", return_value=[line]):
            result = rapid_ocr.ocr_and_match(form_data)
        self.assertEqual(result["matches"][0]["form_bbox"], bbox)

    def test_official_show_information_field_routes_to_wording(self):
        question = (
            "15. SHOW ANY INFORMATION THAT IS BLOWN, BRANDED, OR EMBOSSED "
            "ON THE CONTAINER (e.g., net contents)"
        )
        self.assertEqual(rapid_ocr.field_kind(question), "Wording")

    def test_table_red_wine_accepts_literal_red_wine_label(self):
        lines = [("p3_1.png", [{
            "text": "REDWINE-CONTAINSSULFITES", "conf": 0.99,
            "x1": 1, "y1": 2, "x2": 50, "y2": 12,
        }])]
        result = rapid_ocr.match_class_type(
            rapid_ocr.tagged_corpus(lines),
            "CLASS/TYPE DESCRIPTION\nTABLE RED WINE\n",
        )
        self.assertEqual(result["status"], "MATCH")
        self.assertEqual(result["matched_as"], "red wine")

    def test_government_warning_is_mandatory_final_match(self):
        texts = [
            "GOVERNMENT WARNING: ACCORDING TO THE",
            "SURGEON GENERAL, WOMEN SHOULD NOT DRINK",
            "ALCOHOLIC BEVERAGES DURING PREGNANCY",
            "BECAUSE OF THE RISK OF BIRTH DEFECTS",
            "CONSUMPTION OF ALCOHOLIC BEVERAGES",
            "IMPAIRS YOUR ABILITY TO DRIVE",
            "MAY CAUSE HEALTH PROBLEMS",
        ]
        lines = [("back.png", [{
            "text": text, "conf": 0.99,
            "x1": 1, "y1": i * 10, "x2": 100, "y2": i * 10 + 8,
        } for i, text in enumerate(texts)])]
        result = rapid_ocr.match_government_warning(
            rapid_ocr.tagged_corpus(lines))
        self.assertEqual(result["num"], "GW")
        self.assertTrue(result["mandatory"])
        self.assertEqual(result["status"], "MATCH")
        self.assertIn("SEEN", result["mandatory_message"])

    def test_missing_government_warning_cannot_be_omitted(self):
        result = rapid_ocr.match_government_warning([])
        self.assertEqual(result["status"], "NOT FOUND")
        self.assertEqual(result["mandatory_message"],
                         "NOT SEEN - government warning is mandatory")


if __name__ == "__main__":
    unittest.main()
