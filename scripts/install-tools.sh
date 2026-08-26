#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TOOLS_DIR="$PROJECT_ROOT/.tools"
CACHE_DIR="$TOOLS_DIR/cache"

HOST_TOOLS_URL=${HOST_TOOLS_URL:-https://sophon-file.sophon.cn/sophon-prod-s3/drive/23/03/07/16/host-tools.tar.gz}
HOST_TOOLS_SHA256=ff9a58e8e192b20ea42e1d729c42d2219523209706cb3f0cf134582f6c70f805
HOST_TOOLS_ARCHIVE="$CACHE_DIR/host-tools.tar.gz"
COMPILER="$TOOLS_DIR/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++"

XMAKE_VERSION=3.0.9
XMAKE_URL="https://github.com/xmake-io/xmake/releases/download/v${XMAKE_VERSION}/xmake-bundle-v${XMAKE_VERSION}.linux.x86_64"
XMAKE_SHA256=702c8fafa8585495032ebff6d627afd413e3a5c4bd4eba8f7848f6c02858b1a6
XMAKE="$TOOLS_DIR/bin/xmake"

mkdir -p "$CACHE_DIR" "$TOOLS_DIR/bin"

if [[ ! -x "$COMPILER" ]]; then
    echo "Downloading official LicheeRV Nano host-tools (about 841 MiB)..."
    curl -L --fail --retry 3 --continue-at - -o "$HOST_TOOLS_ARCHIVE" "$HOST_TOOLS_URL"
    printf '%s  %s\n' "$HOST_TOOLS_SHA256" "$HOST_TOOLS_ARCHIVE" | sha256sum -c -
    tar -xzf "$HOST_TOOLS_ARCHIVE" -C "$TOOLS_DIR"
fi

SYSTEM_XMAKE=$(command -v xmake || true)
if [[ -n "$SYSTEM_XMAKE" ]] && "$SYSTEM_XMAKE" --version >/dev/null 2>&1; then
    ln -sfn "$SYSTEM_XMAKE" "$XMAKE"
elif [[ ! -x "$XMAKE" ]] || ! "$XMAKE" --version >/dev/null 2>&1; then
    echo "Installing xmake v${XMAKE_VERSION} locally..."
    local_xmake="$TOOLS_DIR/bin/xmake.bundle"
    curl -L --fail --retry 3 -o "$local_xmake" "$XMAKE_URL"
    printf '%s  %s\n' "$XMAKE_SHA256" "$local_xmake" | sha256sum -c -
    chmod +x "$local_xmake"
    ln -sfn "$local_xmake" "$XMAKE"
fi

echo "Toolchain: $($COMPILER -dumpmachine) / $($COMPILER -dumpversion)"
echo "xmake: $($XMAKE --version | sed -n '1p')"
