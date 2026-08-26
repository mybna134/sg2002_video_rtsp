#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK_ROOT=${NANO_SDK_ROOT:-$PROJECT_ROOT/.sdk/LicheeRV-Nano-Build}

"$PROJECT_ROOT/scripts/prepare-sdk.sh"

(
    cd "$SDK_ROOT"
    set +u
    # shellcheck disable=SC1091
    source build/cvisetup.sh
    defconfig sg2002_licheervnano_sd
    kernel_uapi="$SDK_ROOT/linux_5.10/build/sg2002_licheervnano_sd/riscv/usr/include"
    export CPATH="$kernel_uapi${CPATH:+:$CPATH}"
    build_middleware
)

test -f "$SDK_ROOT/middleware/v2/lib/libsample.so"
test -f "$SDK_ROOT/middleware/v2/lib/libvenc.so"
echo "MMF middleware build completed."
