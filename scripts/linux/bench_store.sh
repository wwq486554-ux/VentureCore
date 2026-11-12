#!/usr/bin/env bash
# Build and run the extension-store microbenchmark with the real MSVC toolchain
# under Wine.
#
#   scripts/linux/bench_store.sh [--rebuild] [--build-type RELEASE|NIGHTLY]
#
# The benchmark (tests/bench_store.cpp) is not part of `run_tests.sh`: it takes a
# few seconds and prints measurements instead of pass/fail.  The numbers are the
# evidence recorded in docs/EngineBindings.md for the store container work, so the
# binary is compiled with the release DLL's code-generation flags - from the same
# scripts/linux/common.sh vcs_cl_flags definition as the DLL, the unit tests and
# the probes (TOOL-01); the run fails if that parity ever breaks.
#
# It links only engine-free sources (the Ext layer plus the stub owner classes of
# the benchmark itself); the toolchain lock is held while compiling so it never
# races the build or another probe.

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$VCS_LINUX_DIR/common.sh"

force=0
build_type="RELEASE"
while (( $# )); do
  case "$1" in
    --rebuild)     force=1 ;;
    --build-type)  build_type="${2:?--build-type needs a value}"; shift ;;
    --build-type=*) build_type="${1#*=}" ;;
    -h|--help)     awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"; exit 0 ;;
    *)             vcs_die "unknown argument: $1" ;;
  esac
  shift
done
[[ $build_type == RELEASE || $build_type == NIGHTLY ]] \
  || vcs_die "--build-type must be RELEASE (default, the shipped configuration) or NIGHTLY (got '$build_type')"

command -v wine >/dev/null 2>&1 || vcs_die "wine is not installed"
vcs_locate_toolchain

src="$VCS_ROOT/tests/bench_store.cpp"
[[ -f $src ]] || vcs_die "missing $src"

work_dir="$VCS_ROOT/Release/IntDir/linux/bench"
mkdir -p "$work_dir"
exe="$work_dir/bench_store.exe"
log="$work_dir/bench_store.build.log"

deps_line="$(grep -m1 '^// *TEST_DEPS:' "$src" | sed -e 's|^// *TEST_DEPS: *||' -e 's|\r$||' || true)"
deps=()
if [[ -n $deps_line ]]; then
  IFS=$' \t' read -r -a deps <<<"$deps_line"
fi
win_sources=("$(vcs_w "$src")")
for dep in "${deps[@]}"; do
  [[ -n $dep ]] || continue
  [[ -f "$VCS_ROOT/$dep" ]] || vcs_die "TEST_DEPS names a missing source: $dep"
  win_sources+=("$(vcs_w "$VCS_ROOT/$dep")")
done

# The released DLL is built with the release flags from common.sh (vcs_cl_flags);
# the benchmark has to measure the same code generation or its numbers describe a
# slower container than the one that ships (D-17, TOOL-01).
vcs_assert_flag_parity Release "$build_type"
mapfile -t cl_flags < <(vcs_cl_flags Release "$build_type" bench)
vcs_info "compile flags: release-aligned (hash $(vcs_flags_hash Release "$build_type" bench))"

vcs_lock_toolchain

gen_dir="$VCS_ROOT/Release/IntDir/Generated"
vcs_generate_git_header "$gen_dir"
vcs_wine_env "$gen_dir"

# Rebuild when any input of the benchmark is newer than the binary: the test
# source, every source in its TEST_DEPS line, any header under src/ or include/
# (they are all on INCLUDE for this translation unit chain) and the generated git
# stamp header that src/Core/Log.cpp's include chain consumes.  Checking only
# tests/bench_store.cpp plus ExtensionStore.h silently measured a stale binary
# after an Ext source/header change (D-17).
needs_build=0
stale_reason=""
if (( force )); then
  needs_build=1; stale_reason="--rebuild"
elif [[ ! -f $exe ]]; then
  needs_build=1; stale_reason="binary missing"
elif [[ $src -nt $exe ]]; then
  needs_build=1; stale_reason="tests/bench_store.cpp changed"
elif [[ $gen_dir/VentureCore.Git.h -nt $exe ]]; then
  needs_build=1; stale_reason="generated git header changed"
else
  for dep in "${deps[@]}"; do
    if [[ $VCS_ROOT/$dep -nt $exe ]]; then
      needs_build=1; stale_reason="$dep changed"; break
    fi
  done
  if (( ! needs_build )); then
    newest_header="$(find "$VCS_ROOT/src" "$VCS_ROOT/include" -type f \
      \( -name '*.h' -o -name '*.hpp' -o -name '*.inl' \) -newer "$exe" \
      -print -quit 2>/dev/null || true)"
    if [[ -n $newest_header ]]; then
      needs_build=1; stale_reason="header changed: ${newest_header#"$VCS_ROOT/"}"
    fi
  fi
fi

if (( needs_build )); then
  vcs_info "building $exe ($stale_reason)"
  set +e
  ( cd "$work_dir" && vcs_wine_run "$VCS_COMPILE_TIMEOUT" wine "$VCS_VC_BIN/cl.exe" \
      "${cl_flags[@]}" "${win_sources[@]}" \
      "/Fe$(vcs_w "$exe")" /link /SUBSYSTEM:CONSOLE /MACHINE:X86 \
      dbghelp.lib onecore.lib ) >"$log" 2>&1
  rc=$?
  set -e
  if (( rc == 124 )); then
    printf 'fatal error: cl.exe timed out after %ss (VCS_COMPILE_TIMEOUT)\n' "$VCS_COMPILE_TIMEOUT" >>"$log"
  fi
  if (( rc != 0 )) || [[ ! -f $exe ]]; then
    sed -n '1,40p' "$log" >&2
    vcs_die "benchmark build failed (exit $rc, see $log)"
  fi
else
  vcs_info "benchmark binary is up to date"
fi

vcs_wine_run "$VCS_PROBE_TIMEOUT" wine "$(vcs_w "$exe")"
