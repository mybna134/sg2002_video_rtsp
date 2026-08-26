#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

"$PROJECT_ROOT/scripts/install-tools.sh"
"$PROJECT_ROOT/scripts/prepare-sdk.sh"
"$PROJECT_ROOT/scripts/fetch-dependencies.sh"

echo "Toolchain, xmake, cvi_rtsp, live555 and Nano SDK are ready."
