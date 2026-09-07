#!/usr/bin/env bash
set -euo pipefail
# Run inside the official pspdev/pspdev container; no ROM is needed.
apk add --no-cache curl python3 py3-yaml build-base cmake xxd zlib-dev openssl-dev
: "${PSPDEV:?PSPDEV must point to the PSP toolchain}"
: "${JOBS:=4}"
repo_root="$PWD"

# Alpine does not package the MIPS GNU binutils target used by the ROM modules.
: "${BINUTILS_VERSION:=2.46.0}"
curl -fsSL "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz" \
	-o "/tmp/binutils-${BINUTILS_VERSION}.tar.xz"
tar -xf "/tmp/binutils-${BINUTILS_VERSION}.tar.xz" -C /tmp
mkdir "/tmp/binutils-${BINUTILS_VERSION}-build"
cd "/tmp/binutils-${BINUTILS_VERSION}-build"
/tmp/binutils-${BINUTILS_VERSION}/configure \
	--target=mips-linux-gnu \
	--prefix=/usr/local \
	--with-system-zlib \
	--disable-gprof \
	--disable-gdb \
	--disable-werror
make -j"$JOBS"
make install
cd "$repo_root"
export PATH="/usr/local/bin:${PATH}"
command -v mips-linux-gnu-ld >/dev/null

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
