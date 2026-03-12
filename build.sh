#!/usr/bin/env bash
# build.sh – build MobicoolFR34 firmware inside Docker
#
# The XC8 compiler is downloaded automatically during `docker build`.
#
# Usage
# -----
#   ./build.sh                      # uses XC8_VERSION default (3.10)
#   XC8_VERSION=3.10 ./build.sh     # select a specific XC8 version

set -euo pipefail

XC8_VERSION="${XC8_VERSION:-3.10}"
IMAGE="mobicool-fr34-builder:xc8-${XC8_VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── 1. Build the Docker image (installer is downloaded automatically) ─────────
echo ">>> Building Docker image: ${IMAGE}"
docker build \
    --build-arg XC8_VERSION="${XC8_VERSION}" \
    -t "${IMAGE}" \
    "${SCRIPT_DIR}"

# ── 2. Compile the firmware ───────────────────────────────────────────────────
echo ">>> Compiling firmware…"
docker run --rm \
    -v "${SCRIPT_DIR}/MobicoolFR34.X:/src" \
    "${IMAGE}" \
    make

# ── 3. Report result ──────────────────────────────────────────────────────────
HEX="${SCRIPT_DIR}/MobicoolFR34.X/dist/default/production/MobicoolFR34.X.production.hex"
if [[ -f "${HEX}" ]]; then
    echo ""
    echo "Build successful!"
    echo "  Output: ${HEX}"
else
    echo ""
    echo "Build finished but HEX file not found at expected location:"
    echo "  ${HEX}"
    exit 1
fi
