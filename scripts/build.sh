#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK_ROOT=${NANO_SDK_ROOT:-$PROJECT_ROOT/.sdk/LicheeRV-Nano-Build}
XMAKE="$PROJECT_ROOT/.tools/bin/xmake"
MODE=${MODE:-release}

"$PROJECT_ROOT/scripts/install-tools.sh"
"$PROJECT_ROOT/scripts/prepare-sdk.sh"
"$PROJECT_ROOT/scripts/fetch-dependencies.sh"

if [[ ! -f "$SDK_ROOT/middleware/v2/lib/libsample.so" || \
      ! -f "$SDK_ROOT/middleware/v2/lib/libvenc.so" ]]; then
    "$PROJECT_ROOT/scripts/build-middleware.sh"
fi

cd "$PROJECT_ROOT"
"$XMAKE" f -c \
    -p cross \
    -a riscv64 \
    -m "$MODE" \
    --sdk="$PROJECT_ROOT/.tools/host-tools/gcc/riscv64-linux-musl-x86_64" \
    --bin="$PROJECT_ROOT/.tools/host-tools/gcc/riscv64-linux-musl-x86_64/bin" \
    --cross=riscv64-unknown-linux-musl- \
    --nano_sdk="$SDK_ROOT" \
    --toolchain_root="$PROJECT_ROOT/.tools/host-tools/gcc/riscv64-linux-musl-x86_64"
"$XMAKE" -vD licheerv-nano-rtsp

echo "Output: $PROJECT_ROOT/build/cross/riscv64/$MODE/licheerv-nano-rtsp"
