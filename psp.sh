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

echo "Removing build/..."
rm -rf build

echo "Building psp-port with $jobs job(s)..."
make -j"$jobs" psp-port "$@"
