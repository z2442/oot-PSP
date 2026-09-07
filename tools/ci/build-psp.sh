#!/usr/bin/env bash
set -euo pipefail
# Run inside the official pspdev/pspdev container; no ROM is needed.
apk add --no-cache python3 py3-yaml build-base cmake xxd zlib-dev openssl-dev
: "${PSPDEV:?PSPDEV must point to the PSP toolchain}"
: "${JOBS:=4}"

# Media Engine audio uses this library in addition to PSPSDK/psp-packages.
# Pin this revision when updating the audio backend's API dependency.
: "${ME_CORE_REF:=3835e12963cbeffee67125e6bf21fd504ac64986}"
git clone https://github.com/mcidclan/psp-media-engine-custom-core.git /tmp/oot-me-core
git -C /tmp/oot-me-core checkout "$ME_CORE_REF"
cmake -S /tmp/oot-me-core -B /tmp/oot-me-core-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/oot-me-core-build --parallel "$JOBS"
cmake --install /tmp/oot-me-core-build
# Require the original font UI in distributed builds.
test -f "$PSPDEV/psp/lib/libintrafont.a"
JOBS="$JOBS" ./psp.sh PYTHON=python3
python3 tools/psp_port_check_assets.py
