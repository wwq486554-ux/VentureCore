#!/usr/bin/env bash
# Build and run the extension-store microbenchmark with the real MSVC toolchain
# under Wine.
#
#   scripts/linux/bench_store.sh [--rebuild]
#
# The benchmark (tests/bench_store.cpp) is not part of `run_tests.sh`: it takes a
# few seconds and prints measurements instead of pass/fail.  The numbers are the
# evidence recorded in docs/EngineBindings.md for the store container work.
#
# It links only engine-free sources (the Ext layer plus the stub owner classes of
# the benchmark itself); the toolchain lock is held while compiling so it never
# races the build or another probe.

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$VCS_LINUX_DIR/common.sh"

force=0
for arg in "$@"; do
  case "$arg" in
    --rebuild) force=1 ;;
    -h|--help) awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"; exit 0 ;;
    *) vcs_die "unknown argument: $arg" ;;
  esac
done

command -v wine >/dev/null 2>&1 || vcs_die "wine is not installed"
vcs_locate_toolchain

src="$VCS_ROOT/tests/bench_store.cpp"
[[ -f $src ]] || vcs_die "missing $src"

work_dir="$VCS_ROOT/Release/IntDir/linux/bench"
mkdir -p "$work_dir"
exe="$work_dir/bench_store.exe"
log="$work_dir/bench_store.build.log"

deps="$(grep -m1 '^// *TEST_DEPS:' "$src" | sed -e 's|^// *TEST_DEPS: *||' -e 's|\r$||')"
win_sources=("$(vcs_w "$src")")
for dep in $deps; do
  [[ -f "$VCS_ROOT/$dep" ]] || vcs_die "TEST_DEPS names a missing source: $dep"
  win_sources+=("$(vcs_w "$VCS_ROOT/$dep")")
done

cl_flags=(
  /nologo /W4 /WX- /MT /EHsc- /Gz /std:c++20
  /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000
  /wd4100 /wd4201 /wd4458 /wd4819
)

vcs_lock_toolchain

gen_dir="$VCS_ROOT/Release/IntDir/Generated"
vcs_generate_git_header "$gen_dir"
vcs_wine_env "$gen_dir"

if (( force )) || [[ ! -f $exe ]] || [[ $src -nt $exe ]] || [[ $VCS_ROOT/src/Ext/ExtensionStore.h -nt $exe ]]; then
  vcs_info "building $exe"
  set +e
  ( cd "$work_dir" && wine "$VCS_VC_BIN/cl.exe" "${cl_flags[@]}" "${win_sources[@]}" \
      "/Fe$(vcs_w "$exe")" /link /SUBSYSTEM:CONSOLE /MACHINE:X86 \
      dbghelp.lib onecore.lib ) >"$log" 2>&1
  rc=$?
  set -e
  if (( rc != 0 )) || [[ ! -f $exe ]]; then
    sed -n '1,40p' "$log" >&2
    vcs_die "benchmark build failed (see $log)"
  fi
else
  vcs_info "benchmark binary is up to date"
fi

wine "$(vcs_w "$exe")"
