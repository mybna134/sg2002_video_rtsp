#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CACHE_DIR="$PROJECT_ROOT/.tools/cache"
CVI_RTSP_DIR="$PROJECT_ROOT/third_party/cvi_rtsp"
LIVE555_DIR="$PROJECT_ROOT/third_party/live555"
SDK_ROOT=${NANO_SDK_ROOT:-$PROJECT_ROOT/.sdk/LicheeRV-Nano-Build}

CVI_RTSP_COMMIT=58c825fa538731e7c230a1379afd2f33cf74592c
LIVE555_VERSION=2020.07.21
LIVE555_SHA256=1a73bfc3a0eff609d9466e730e03091296a0116fa2a823b76d97235b64f39f4c
LIVE555_ARCHIVE="$SDK_ROOT/oss/oss_release_tarball/musl_riscv64/live555.tar.gz"

mkdir -p "$CACHE_DIR" "$PROJECT_ROOT/third_party"

if [[ ! -d "$CVI_RTSP_DIR/.git" ]]; then
    git clone --filter=blob:none --no-checkout https://github.com/sophgo/cvi_rtsp.git "$CVI_RTSP_DIR"
fi
if ! git -C "$CVI_RTSP_DIR" cat-file -e "$CVI_RTSP_COMMIT^{commit}" 2>/dev/null; then
    git -C "$CVI_RTSP_DIR" fetch --depth 1 origin "$CVI_RTSP_COMMIT"
fi
git -C "$CVI_RTSP_DIR" checkout --detach "$CVI_RTSP_COMMIT"

if [[ ! -f "$LIVE555_ARCHIVE" ]]; then
    echo "Missing SDK live555 archive; run scripts/prepare-sdk.sh first." >&2
    exit 1
fi

if [[ ! -f "$LIVE555_DIR/include/liveMedia/liveMedia.hh" ]]; then
    printf '%s  %s\n' "$LIVE555_SHA256" "$LIVE555_ARCHIVE" | sha256sum -c -
    rm -rf "$LIVE555_DIR"
    mkdir -p "$LIVE555_DIR"
    tar -xzf "$LIVE555_ARCHIVE" -C "$LIVE555_DIR"
fi

echo "cvi_rtsp: $(git -C "$CVI_RTSP_DIR" rev-parse --short HEAD)"
echo "live555: $LIVE555_VERSION"
