# Deployment and recovery

The current VM is CentOS Stream 9 at `/home/vpcuser/cola-qa`. The application
is managed by `cola-qa.service`; the Cloudflare quick tunnel has a separate
`cloudflared-tunnel.service`. Do not use broad `pkill` commands.

## Build a locked Python environment

Python 3.11 is the deployment contract:

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

Run the unit tests and the fixed sample before replacing the active binary or
environment. Keep the previous active binary and environment until the new
service returns healthy and the sample result is compared.

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

## Deployment receipt

Record the source commit in the Git worktree before deployment:

```bash
git rev-parse HEAD
```

Then record the deployed hashes and runtime on the VM:

```bash
sha256sum web/requirements.lock web/app.py web/rapid_ocr.py \
  web/templates/index.html native/cola_label_qa.cpp
sha256sum bin/cola_label_qa samples/sample-cola.pdf
.venv/bin/python --version
.venv/bin/python -m pip freeze
```

The service creates run data under `web_runs/`. Run data is disposable after
its retention period; source PDFs used for regression must live under
`samples/regression/` with hashes in the manifest.

## Rollback

Rollback means restoring the single previous accepted source snapshot, binary,
and virtual environment together, then restarting `cola-qa.service`. Never mix
an older binary with unrecorded source or dependency state.
