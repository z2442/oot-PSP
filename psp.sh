#!/usr/bin/env bash
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

if [[ ! -f Makefile ]]; then
    echo "error: Makefile not found in $repo_root" >&2
    exit 1
fi

if [[ -n "${CLEAN_EXTRACTED:-}" ]]; then
    echo "Removing extracted/..."
    rm -rf extracted
fi

if [[ -n "${JOBS:-}" ]]; then
    jobs="$JOBS"
elif command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
else
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
fi

PSP_ENABLE_GPROF="${PSP_ENABLE_GPROF:-0}"
if [[ "${GPROF:-0}" == "1" ]]; then
    PSP_ENABLE_GPROF=1
fi

case "$PSP_ENABLE_GPROF" in
    1|ON|on|TRUE|true|YES|yes)
        PSP_ENABLE_GPROF=1
        ;;
    0|OFF|off|FALSE|false|NO|no|"")
        PSP_ENABLE_GPROF=0
        ;;
    *)
        echo "error: unsupported PSP_ENABLE_GPROF value: $PSP_ENABLE_GPROF" >&2
        exit 1
        ;;
esac

echo "Removing build/..."
rm -rf build

if [[ "$PSP_ENABLE_GPROF" == "1" ]]; then
    echo "Building psp-port gprof mode with $jobs job(s)..."
else
    echo "Building psp-port with $jobs job(s)..."
fi

make -j"$jobs" psp-port PSP_ENABLE_GPROF="$PSP_ENABLE_GPROF" "$@"
