#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
ROOT_DIR="$( cd "$SCRIPT_DIR/.." >/dev/null 2>&1 && pwd )"
cd "$ROOT_DIR"

PYTHON_BIN="$ROOT_DIR/.venv/bin/python"
BINARY_PATH="$ROOT_DIR/bin/cola_label_qa"

if [ ! -x "$PYTHON_BIN" ]; then
    echo "Missing .venv. Create it with Python 3.11 and install web/requirements.lock." >&2
    exit 2
fi

"$PYTHON_BIN" -m pip check

if [ "$(uname -s)" = "Darwin" ] && [ -z "${MUPDF_ROOT:-}" ]; then
    MUPDF_ROOT="$(brew --prefix mupdf)"
    export MUPDF_ROOT
fi

needs_build=false
if [ ! -x "$BINARY_PATH" ]; then
    needs_build=true
elif [ -n "$(find "$ROOT_DIR/native" -type f -newer "$BINARY_PATH" -print -quit)" ]; then
    needs_build=true
fi

if [ "$needs_build" = true ]; then
    echo "==> Building C++ analyzer..."
    cmake -S "$ROOT_DIR/native" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE=Release -DMUPDF_ROOT="${MUPDF_ROOT:-}"
    cmake --build "$ROOT_DIR/build" --parallel
    mkdir -p "$ROOT_DIR/bin"
    install -m 0755 "$ROOT_DIR/build/cola_label_qa" "$BINARY_PATH"
fi

PORT="${PORT:-8080}"
echo "==> Launching COLA QA Web Application on http://localhost:$PORT"
exec "$PYTHON_BIN" -m uvicorn web.app:app --host 127.0.0.1 --port "$PORT"
