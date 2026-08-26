#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK_ROOT=${NANO_SDK_ROOT:-$PROJECT_ROOT/.sdk/LicheeRV-Nano-Build}
SDK_COMMIT=d4003f15b35d43ad4842f427050ab2bba0114fa5

"$PROJECT_ROOT/scripts/install-tools.sh"

if [[ -z ${NANO_SDK_ROOT:-} ]]; then
    if [[ ! -d "$SDK_ROOT/.git" ]]; then
        mkdir -p "$(dirname -- "$SDK_ROOT")"
        git clone --filter=blob:none --sparse https://github.com/sipeed/LicheeRV-Nano-Build.git "$SDK_ROOT"
    fi

    if ! git -C "$SDK_ROOT" cat-file -e "$SDK_COMMIT^{commit}" 2>/dev/null; then
        git -C "$SDK_ROOT" fetch --depth 1 origin "$SDK_COMMIT"
    fi
    git -C "$SDK_ROOT" checkout --detach "$SDK_COMMIT"
    git -C "$SDK_ROOT" sparse-checkout set \
        build \
        middleware \
        osdrv/interdrv/v2 \
        ramdisk/rootfs \
        oss/oss_release_tarball/musl_riscv64 \
        linux_5.10/include \
        linux_5.10/drivers/staging/android/uapi \
        isp_tuning
elif [[ ! -f "$SDK_ROOT/build/cvisetup.sh" ]]; then
    echo "NANO_SDK_ROOT is not a LicheeRV Nano SDK: $SDK_ROOT" >&2
    exit 1
fi

if [[ ! -e "$SDK_ROOT/host-tools" ]]; then
    ln -s "$PROJECT_ROOT/.tools/host-tools" "$SDK_ROOT/host-tools"
fi

(
    cd "$SDK_ROOT"
    set +u
    # shellcheck disable=SC1091
    source build/cvisetup.sh
    defconfig sg2002_licheervnano_sd

    kernel_uapi="$SDK_ROOT/linux_5.10/build/sg2002_licheervnano_sd/riscv/usr/include"
    mkdir -p "$kernel_uapi/linux"
    cp -a osdrv/interdrv/v2/include/common/uapi/linux/. "$kernel_uapi/linux/"
    cp -a osdrv/interdrv/v2/include/chip/mars/uapi/linux/. "$kernel_uapi/linux/"
    cp -a linux_5.10/drivers/staging/android/uapi/ion.h "$kernel_uapi/linux/"
    cp -a linux_5.10/drivers/staging/android/uapi/ion_cvitek.h "$kernel_uapi/linux/"
    cp -a linux_5.10/include/uapi/linux/dma-buf.h "$kernel_uapi/linux/"

    mkdir -p "$OSS_TARBALL_PATH"
    cp -a oss/oss_release_tarball/musl_riscv64/. "$OSS_TARBALL_PATH/"
)

echo "Nano SDK prepared at $SDK_ROOT"
