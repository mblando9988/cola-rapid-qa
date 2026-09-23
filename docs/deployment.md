# Deployment and recovery

The current VM is CentOS Stream 9 at `/home/vpcuser/cola-qa`. The application
is managed by `cola-qa.service`; the Cloudflare quick tunnel has a separate
`cloudflared-tunnel.service`. Do not use broad `pkill` commands.

## Build a locked Python environment

The server uses Python 3.11:

```bash
cd /home/vpcuser/cola-qa
python3.11 -m venv .venv.next
.venv.next/bin/python -m pip install --upgrade pip
.venv.next/bin/python -m pip install -r web/requirements.lock
.venv.next/bin/python -m pip check
```

Build the C++ analyzer against the preserved MuPDF source tree:

```bash
cmake -S native -B build -DCMAKE_BUILD_TYPE=Release \
  -DMUPDF_ROOT=/home/vpcuser/mupdf-src \
  -DTESSERACT_ROOT=/usr/local
cmake --build build --parallel
install -d bin
install -m 0755 build/cola_label_qa bin/cola_label_qa
```

Run the unit tests and the sample before swapping in the new binary or
environment. Keep the old binary and environment until the new service is
healthy and its sample result matches.

## Install the service

```bash
sudo install -m 0644 deploy/cola-qa.service /etc/systemd/system/cola-qa.service
sudo systemctl daemon-reload
sudo systemctl enable --now cola-qa.service
sudo systemctl restart cola-qa.service
sudo systemctl status --no-pager cola-qa.service
curl --fail --show-error http://127.0.0.1:8081/api/health
```

Restart only the tunnel when necessary:

```bash
sudo systemctl restart cloudflared-tunnel.service
sudo journalctl -u cloudflared-tunnel.service -n 50 --no-pager
```

## Record what was deployed

Before deploying, note the source commit:

```bash
git rev-parse HEAD
```

Then note the file hashes and Python setup on the VM:

```bash
sha256sum web/requirements.lock web/app.py web/rapid_ocr.py \
  web/templates/index.html native/cola_label_qa.cpp
sha256sum bin/cola_label_qa samples/sample-cola.pdf
.venv/bin/python --version
.venv/bin/python -m pip freeze
```

The service writes run data to `web_runs/`, which can be cleared after the
retention period. PDFs used for regression tests go in `samples/regression/`
with their hashes in the manifest.

## Rollback

To roll back, restore the previous source, binary and virtual environment
together, then restart `cola-qa.service`. Don't mix an old binary with newer
source or dependencies.
