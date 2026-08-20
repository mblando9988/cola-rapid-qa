# Multi-stage reproducible runtime for the C++ extractor and Python web service.
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    libmupdf-dev \
    libtesseract-dev \
    libfreetype-dev \
    libgumbo-dev \
    libharfbuzz-dev \
    libjbig2dec0-dev \
    libjpeg62-turbo-dev \
    libmujs-dev \
    libopenjp2-7-dev \
    zlib1g-dev \
    tesseract-ocr-eng \
    tesseract-ocr-ita \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY native ./native

RUN cmake -S native -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc)

# Final Runtime Image
FROM python:3.11-slim-bookworm

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONUNBUFFERED=1
ENV PORT=8080

RUN apt-get update && apt-get install -y --no-install-recommends \
    libmupdf-dev \
    libtesseract5 \
    libfreetype6 \
    libgl1 \
    libglib2.0-0 \
    libgumbo1 \
    libharfbuzz0b \
    libjbig2dec0 \
    libmujs2 \
    tesseract-ocr-eng \
    tesseract-ocr-ita \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY web/requirements.lock ./web/requirements.lock
RUN python -m pip install --no-cache-dir -r web/requirements.lock \
    && python -m pip check

# Copy binary from builder
COPY --from=builder /app/build/cola_label_qa ./bin/cola_label_qa
COPY web ./web
RUN useradd --create-home --uid 10001 cola \
    && mkdir -p /app/samples /app/web_runs /app/bin \
    && chown -R cola:cola /app

USER cola

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=30s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/api/health', timeout=3)"

CMD ["python", "-m", "uvicorn", "web.app:app", "--host", "0.0.0.0", "--port", "8080"]
