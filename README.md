# COLA Rapid QA

Compare a TTB COLA application with the text printed on its label images.

**https://cola-rapid-qa.vercel.app/**

## Approach

This project does not use a large cloud vision model to read labels.

Sending every document to a big vision model adds cost and delay, and it can
give a confident wrong answer. Those models help with hard or unusual images.
For checking the same plain text on label after label, OCR is enough.

The demo does five things:

1. Read the COLA application PDF.
2. Extract the label images included with it.
3. Read the label text with OCR.
4. Compare that text with the important information on the application.
5. Show the worker the evidence for each result.

It does not approve or reject labels or make compliance decisions. It saves
the reviewer from hunting for the same fields by hand, and the reviewer still
makes the call.

## Next steps

- Better cleanup of blurry or low-quality label images, including a trained
  super-resolution step.
- Easier drag-and-drop and a faster review screen.
- Hooking it into the office's existing .NET systems.

## Try the demo

Click **Try synthetic demo** to run the whole workflow on made-up application
data. The `7/7` result comes from live OCR and matching, not a fixed value.

![Synthetic demo workflow](docs/screenshots/demo-workflow.jpg)

## Read a result

Each row compares one application field with label OCR evidence. Click an ID to
highlight the corresponding text on the application and label.

![Application field compared with label OCR](docs/screenshots/field-comparison.jpg)

Every analysis ends with `#GW`. This required row reports whether the government
warning is **Seen**, **Incomplete**, or **Not seen**.

![Mandatory government warning check](docs/screenshots/government-warning-detail.jpg)

## Hard cases

If a match has a low confidence score, looks wrong, or needs a closer look,
click **JSON** to download that analysis record.

![Export a difficult result as JSON for review](docs/screenshots/json-feedback-export.png)

The file has the application fields, OCR text, match results and confidence
scores for that case. A person marks what was right or wrong, and only those
checked files go into the test or training set. That way the tool gets better
from the office's own hard cases, on the office's schedule.

## Layout

- `native/`: C++ analyzer and its bundled headers
- `web/`: FastAPI app, OCR matcher, UI template and Python requirements
- `scripts/`: local start scripts
- `deploy/`: Docker Compose, Dockerfile and systemd files
- `docs/`: deployment notes and screenshots

## Run it

You need Python 3.11, CMake, MuPDF, and Tesseract installed first.

Run these commands one at a time in Terminal:

```bash
# Make a private Python setup for this project
python3.11 -m venv .venv

# Install what the website needs
.venv/bin/python -m pip install -r web/requirements.lock

# Build the label-reading program
MUPDF_ROOT="$(brew --prefix mupdf)" cmake -S native -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
install -d bin
install -m 0755 build/cola_label_qa bin/cola_label_qa

# Start the website
PORT=8080 ./scripts/start_web.sh
```

Then open `http://127.0.0.1:8080`.

## Check that it works

```bash
# Run the tests
.venv/bin/python -m unittest discover -s tests -v

# Try the included example
./bin/cola_label_qa samples/demo-cola.pdf --task both --json --out build/sample-images
```

The tests should finish with `OK`. The example creates extracted label images
in `build/sample-images`.

## Notes

- The demo uses a real [TTB F 5100.31](https://www.ttb.gov/system/files/images/pdfs/forms/f510031.pdf) form with made-up example information.
- PDFs you upload and generated results are not publicly available.
- Windows has not been tested yet.
- Server setup and recovery instructions are in [the deployment guide](docs/deployment.md).
