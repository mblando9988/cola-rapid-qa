# COLA Rapid QA

Compare a TTB COLA application with the text printed on its label images.

Live demo: https://cola-rapid-qa.vercel.app/

## Approach

Labels are read with local OCR (RapidOCR in the web app, Tesseract in the C++
tool). Label text is plain print, so a cloud vision model would add cost and
delay without much benefit, and it can give a confident wrong answer.

How it works:

1. Read the COLA application PDF.
2. Extract the label images included with it.
3. Read the label text with OCR.
4. Compare that text with the important information on the application.
5. Show the worker the evidence for each result.

It doesn't approve or reject anything. It lines up each application field with
the label text so a reviewer can check it faster.

## Next steps

- Better cleanup of blurry or low-quality label images, including a trained
  super-resolution step.
- Easier drag-and-drop and a faster review screen.
- Hooking it into the office's existing .NET systems.

## Try the demo

Click **Try synthetic demo** to run the whole workflow on made-up application
data. The 7/7 score is computed by the OCR and matcher on every run.

![Synthetic demo workflow](docs/screenshots/demo-workflow.jpg)

## Read a result

Each row compares one application field with label OCR evidence. Click an ID to
highlight the corresponding text on the application and label.

![Application field compared with label OCR](docs/screenshots/field-comparison.jpg)

Every analysis ends with `#GW`. This required row reports whether the government
warning is **Seen**, **Incomplete**, or **Not seen**.

![Mandatory government warning check](docs/screenshots/government-warning-detail.jpg)

## Hard cases

To save a questionable result, click **JSON** to download its analysis record.

![Export a difficult result as JSON for review](docs/screenshots/json-feedback-export.png)

The file has the application fields, OCR text, match results and confidence
scores for that case. Have a person mark what was right or wrong before any of
these files go into a test or training set, so the tool improves from the
office's own hard cases.

## Layout

- `native/`: C++ analyzer and its bundled headers
- `web/`: FastAPI app, OCR matcher, UI template and Python requirements
- `scripts/`: local start scripts
- `deploy/`: Docker Compose, Dockerfile and systemd files
- `docs/`: deployment notes and screenshots

## Run it

macOS with Homebrew. Install Python 3.11, CMake, MuPDF and Tesseract first.

```bash
# create a virtualenv
python3.11 -m venv .venv

# install Python dependencies (includes RapidOCR)
.venv/bin/python -m pip install -r web/requirements.lock

# build the C++ analyzer
MUPDF_ROOT="$(brew --prefix mupdf)" cmake -S native -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
install -d bin
install -m 0755 build/cola_label_qa bin/cola_label_qa

# start the web app
PORT=8080 ./scripts/start_web.sh
```

Then open `http://127.0.0.1:8080`.

## Check that it works

```bash
# Run the tests
.venv/bin/python -m unittest discover -s tests -v

# run the C++ analyzer on the demo PDF
./bin/cola_label_qa samples/demo-cola.pdf --task both --json --out build/sample-images
```

The tests should finish with `OK`. The example creates extracted label images
in `build/sample-images`.

## Notes

- The demo uses a real [TTB F 5100.31](https://www.ttb.gov/system/files/images/pdfs/forms/f510031.pdf) form with made-up example information.
- PDFs you upload and generated results are not publicly available.
- Windows has not been tested yet.
- Server setup and recovery instructions are in [the deployment guide](docs/deployment.md).
