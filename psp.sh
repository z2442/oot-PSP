#!/usr/bin/env bash
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

host_os="$(uname -s)"

# PSPSDK's tools are installed in $PSPDEV/bin. Make the common setup work even
# when PSPDEV is exported but its bin directory was not added to PATH.
if [[ -n "${PSPDEV:-}" && -d "${PSPDEV}/bin" ]]; then
    case ":${PATH}:" in
        *":${PSPDEV}/bin:"*) ;;
        *) export PATH="${PSPDEV}/bin:${PATH}" ;;
    esac
fi

# macOS ships GNU Make 3.81 as `make`, which is too old for this Makefile.
# Homebrew installs a current GNU Make as `gmake`.
if [[ -n "${MAKE:-}" ]]; then
    make_cmd="$MAKE"
elif [[ "$host_os" == "Darwin" ]]; then
    make_cmd="gmake"
else
    make_cmd="make"
fi

if ! command -v "$make_cmd" >/dev/null 2>&1; then
    if [[ "$host_os" == "Darwin" && "$make_cmd" == "gmake" ]]; then
        echo "error: GNU Make is required; install it with 'brew install make'" >&2
    else
        echo "error: unable to find build tool: $make_cmd" >&2
    fi
    exit 1
fi

for tool in psp-config psp-gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: unable to find $tool; install PSPSDK and add \$PSPDEV/bin to PATH" >&2
        exit 1
    fi
done

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

# Query the effective configuration with exactly the same overrides as the build.
make_args=(PSP_ENABLE_GPROF="$PSP_ENABLE_GPROF" "$@")
read_make_variable() {
    local value
    value="$("$make_cmd" --no-print-directory "${make_args[@]}" "print-$1")"
    value="${value#* set to [}"
    printf '%s\n' "${value%]}"
}
read -r -a psp_version_list <<< "$(read_make_variable PSP_PORT_VERSIONS)"
bundle_dir="$(read_make_variable PSP_PORT_INSTALL_DIR)"
if [[ -z "$bundle_dir" || "${#psp_version_list[@]}" -eq 0 ]]; then
    echo "error: the Makefile did not report a bundle directory and revisions" >&2
    exit 1
fi

if [[ "${CLEAN_BUILD:-0}" == "1" ]]; then
    echo "Removing build/..."
    rm -rf build
fi

if [[ "$PSP_ENABLE_GPROF" == "1" ]]; then
    echo "Building complete PSP bundle in gprof mode with $jobs job(s) using $make_cmd..."
else
    echo "Building complete PSP bundle with $jobs job(s) using $make_cmd..."
fi
echo "Game modules: ${#psp_version_list[@]} (${psp_version_list[*]})"

"$make_cmd" -j"$jobs" psp-port "${make_args[@]}"

required_files=(
    EBOOT.PBP
    Modules/unpacker.prx
    Plugins/dvemgr.prx
)
for version in "${psp_version_list[@]}"; do
    required_files+=("Modules/$version.prx")
done

missing_files=()
for relative_path in "${required_files[@]}"; do
    if [[ ! -f "$bundle_dir/$relative_path" ]]; then
        missing_files+=("$relative_path")
    fi
done

if [[ "${#missing_files[@]}" -ne 0 ]]; then
    echo "error: PSP bundle is incomplete in $bundle_dir" >&2
    printf '  missing: %s\n' "${missing_files[@]}" >&2
    exit 1
fi

echo "Complete PSP bundle written to $bundle_dir"
echo "  launcher: EBOOT.PBP"
echo "  runtime unpacker: Modules/unpacker.prx"
echo "  game modules: ${#psp_version_list[@]}"
echo "  DVE plugin: Plugins/dvemgr.prx"
