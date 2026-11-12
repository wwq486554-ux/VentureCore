#!/usr/bin/env bash
# Compile and run the platform's unit tests with the real MSVC toolchain under
# Wine.
#
#   scripts/linux/run_tests.sh [options] [FILTER]
#
# Every `tests/test_*.cpp` is a standalone console program.  It is compiled
# together with the non-engine sources it names in a `TEST_DEPS:` comment on one
# of its first lines:
#
#   // TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp
#
# so a test never has to link the engine-facing sources (which need the game
# process).  Tests print their own diagnostics and exit 0 on success; anything
# else is a failure.
#
# Build configuration (TOOL-01): the compiler options come from
# scripts/linux/common.sh's vcs_cl_flags - the single definition shared with the
# DLL build, the benchmark and the probes.  The tests are compiled with the
# *release* code-generation flags and the same preprocessor definitions as the
# shipped VentureCore.dll (same optimisation/stack-protector/RTTI settings, no
# C++ exceptions, NDEBUG in Release), so a green test run actually describes the
# shipped binary.  vcs_assert_flag_parity fails the run if the test profile ever
# diverges from the build profile in anything but the documented debug-info
# format; the flag hash is printed below.  Because NDEBUG is on, assert() is
# compiled out of the test binaries exactly as in the DLL: use the Check()/FAIL()
# conventions of the suite, never assert(), to verify behaviour.
#
# Options:
#   --list         list the discovered tests and exit
#   --verbose      echo the compiler command line and print each test's run output
#   --no-run       only compile, do not execute (no completion-marker check)
#   --build-type T build type macro for the test build: RELEASE (default, the
#                  shipped configuration) or NIGHTLY; matches the DLL build
#   -h, --help
#
# Completion marker (TOOL-12): a test counts as passed only when it exits 0,
# its output contains the suite's stable final marker (`PASS (0 failures)` /
# `PASS (1 failure)` or `test_<name>: all checks passed`, trailing CR allowed)
# and it printed at least one per-check line starting with `ok `.  Exiting 0
# alone used to be enough, so a test whose Check() calls were deleted would
# still "pass"; now it is reported as "no completion marker" / "no checks".
#
# Each compiler invocation and each test binary runs under a wall-clock limit
# (VCS_COMPILE_TIMEOUT / VCS_TEST_TIMEOUT).  The runner holds the shared
# toolchain lock, and a hung binary would otherwise block every later build
# until the lock timeout.
#
# The toolchain lock from common.sh is held while compiling, so this can run
# next to a build without corrupting the shared Wine prefix - but it does not
# make a concurrent *build* safe against a partially edited tree; do not edit
# sources while testing.
#
# Notes:
#   * `main` is declared `int __cdecl main(void)` because the platform is built
#     with /Gz (stdcall default), exactly like the load/ABI probes;
#   * headers come from src/, include/ and the external/YRpp include path (the
#     production TUs linked by the integration tests include <Syringe.h>), but
#     no engine ABI type is needed and gamemd.exe does not have to be present:
#     the engine-facing hooks stay in load_probe.sh / probe_hosts.sh.

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$VCS_LINUX_DIR/common.sh"

usage() { awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"; }

verbose=0
no_run=0
list_only=0
filter=""
build_type="RELEASE"

while (( $# )); do
  case "$1" in
    --list)       list_only=1 ;;
    --verbose)    verbose=1 ;;
    --no-run)     no_run=1 ;;
    --build-type) build_type="${2:?--build-type needs a value}"; shift ;;
    --build-type=*) build_type="${1#*=}" ;;
    -h|--help)    usage; exit 0 ;;
    -*)           vcs_die "unknown argument: $1 (try --help)" ;;
    *)            filter="$1" ;;
  esac
  shift
done
[[ $build_type == RELEASE || $build_type == NIGHTLY ]] \
  || vcs_die "--build-type must be RELEASE (default, the shipped configuration) or NIGHTLY (got '$build_type')"

# Each test binary runs under its own wall-clock limit (the compile step uses
# VCS_COMPILE_TIMEOUT).  A hung test is reported as a failure instead of
# blocking every later build on the shared toolchain lock (TOOL-10/TOOL-12).
test_timeout="${VCS_TEST_TIMEOUT:-120}"
if ! [[ $test_timeout =~ ^[0-9]+$ ]] || (( test_timeout <= 0 )); then
  vcs_die "VCS_TEST_TIMEOUT must be a positive number of seconds (got '$test_timeout')"
fi

tests_dir="$VCS_ROOT/tests"
# A missing or empty tests/ is a failing configuration, not a pass: the runner is
# used by CI and by the release checklist, and exiting 0 would report "all tests
# passed" when none ran (D-13).
[[ -d $tests_dir ]] || vcs_die "no tests/ directory in $VCS_ROOT"

mapfile -t all_tests < <(find "$tests_dir" -maxdepth 1 -type f -name 'test_*.cpp' -printf '%f\n' | sort)
(( ${#all_tests[@]} > 0 )) || vcs_die "tests/ contains no test_*.cpp - nothing to run"

selected=()
for t in "${all_tests[@]}"; do
  if [[ -z $filter || $t == *"$filter"* ]]; then
    selected+=("$t")
  fi
done
(( ${#selected[@]} > 0 )) || vcs_die "no test matches '$filter'"

if (( list_only )); then
  printf '%s\n' "${selected[@]}"
  exit 0
fi

command -v wine >/dev/null 2>&1 || vcs_die "wine is not installed"
vcs_locate_toolchain

work_dir="$VCS_ROOT/Release/IntDir/linux/tests"
gen_dir="$VCS_ROOT/Release/IntDir/Generated"
mkdir -p "$work_dir"

# TOOL-01: the test build must use the shipped DLL's flags.  The assert is a
# real machine check (profile hashes), not a comment, and the resulting hash is
# printed so the acceptance log can be compared with the build log.
vcs_assert_flag_parity Release "$build_type"
flags_hash="$(vcs_flags_hash Release "$build_type" test)"
vcs_info "compile flags: release-aligned (hash $flags_hash, definition: scripts/linux/common.sh vcs_cl_flags)"
mapfile -t cl_flags < <(vcs_cl_flags Release "$build_type" test)

failed=0
passed=0
total_checks=0
declare -a failed_names=()
declare -a warning_tests=()
total_warnings=0

vcs_lock_toolchain

# Tests compile the platform sources directly, so the generated git stamp header
# that src/Core/PlatformVersion.h includes has to exist and be on INCLUDE.
vcs_generate_git_header "$gen_dir"
vcs_wine_env "$gen_dir"

# Marks a test as passed only when its output carries the suite's completion
# marker and at least one executed check (TOOL-12).  Prints the check count and a
# failure reason through the globals below.
parse_test_log() { # $1 = test name, $2 = run log; sets test_checks/test_marker_ok
  local name="$1" log="$2"
  test_checks="$(grep -acE '^ok ' "$log" 2>/dev/null || true)"
  test_checks="${test_checks:-0}"
  test_marker_ok=0
  if grep -aqiE '^PASS \([0-9]+ failures?\)' "$log" 2>/dev/null; then
    test_marker_ok=1
  elif grep -aqiE "^test_[A-Za-z0-9_]+: all checks passed" "$log" 2>/dev/null; then
    test_marker_ok=1
  fi
  if grep -aqE '^FAIL ' "$log" 2>/dev/null; then
    test_marker_ok=0
  fi
}

for test in "${selected[@]}"; do
  name="${test%.cpp}"
  src="$tests_dir/$test"
  exe="$work_dir/$name.exe"
  log="$work_dir/$name.build.log"

  # TEST_DEPS: one comment line with repo-relative source paths.  A test without
  # the line is valid (it links only itself), so a no-match grep must not abort
  # the run under "set -o pipefail" - that used to fail the whole runner without
  # a message for such a test.  The list is read with `read -r -a` so that
  # quoting/word-splitting mistakes cannot silently split a dependency path
  # (TOOL-14); paths with spaces are not supported by this convention.
  deps_line="$(grep -m1 '^// *TEST_DEPS:' "$src" | sed -e 's|^// *TEST_DEPS: *||' -e 's|\r$||' || true)"
  sources=("$src")
  if [[ -n $deps_line ]]; then
    IFS=$' \t' read -r -a deps <<<"$deps_line"
    for dep in "${deps[@]}"; do
      [[ -n $dep ]] || continue
      [[ -f "$VCS_ROOT/$dep" ]] || vcs_die "$test: TEST_DEPS names a missing source: $dep"
      sources+=("$VCS_ROOT/$dep")
    done
  fi
  win_sources=()
  for s in "${sources[@]}"; do win_sources+=("$(vcs_w "$s")"); done

  if (( verbose )); then
    vcs_info "CC $test${deps_line:+ (+ $deps_line)}"
  else
    printf '  [build] %-34s' "$test"
  fi

  set +e
  ( cd "$work_dir" && vcs_wine_run "$VCS_COMPILE_TIMEOUT" wine "$VCS_VC_BIN/cl.exe" \
      "${cl_flags[@]}" "${win_sources[@]}" \
      "/Fe$(vcs_w "$exe")" /link /SUBSYSTEM:CONSOLE /MACHINE:X86 \
      dbghelp.lib onecore.lib ) >"$log" 2>&1
  compile_rc=$?
  set -e

  if (( compile_rc == 124 )); then
    (( verbose )) || printf 'FAIL (compile timeout after %ss)\n' "$VCS_COMPILE_TIMEOUT"
    printf 'fatal error: cl.exe timed out after %ss (VCS_COMPILE_TIMEOUT)\n' "$VCS_COMPILE_TIMEOUT" >>"$log"
    sed -n '1,40p' "$log" >&2
    failed=$((failed + 1))
    failed_names+=("$name (compile timeout)")
    continue
  fi
  if (( compile_rc != 0 )) || [[ ! -f $exe ]]; then
    (( verbose )) || printf 'FAIL (compile)\n'
    sed -n '1,40p' "$log" >&2
    failed=$((failed + 1))
    failed_names+=("$name (compile)")
    continue
  fi
  (( verbose )) || printf 'ok\n'

  # A test compiles with /W4 and warnings do not fail the build (/WX-), so
  # surface them even when the compile succeeds - otherwise a new warning in a
  # test is invisible until someone opens the build log.  (-a: MSVC's localized
  # messages make the log look like a binary file to grep.)
  test_warnings="$(grep -acE 'warning C[0-9]+' "$log" 2>/dev/null || true)"
  if [[ ${test_warnings:-0} -gt 0 ]]; then
    total_warnings=$((total_warnings + test_warnings))
    warning_tests+=("$name")
    (( verbose )) || printf '      %d compiler warning(s)\n' "$test_warnings"
    grep -aE 'warning C[0-9]+' "$log" 2>/dev/null | sed -n '1,3p' | sed 's/^/      /' >&2 || true
  fi

  if (( no_run )); then
    passed=$((passed + 1))
    continue
  fi

  run_log="$work_dir/$name.run.log"
  set +e
  ( cd "$work_dir" && vcs_wine_run "$test_timeout" wine "$(vcs_w "$exe")" ) >"$run_log" 2>&1
  run_rc=$?
  set -e

  if (( run_rc == 124 )); then
    failed=$((failed + 1))
    failed_names+=("$name (timeout after ${test_timeout}s)")
    printf '  [run]   %-34s FAIL (timeout after %ss)\n' "$test" "$test_timeout"
    sed -n '1,80p' "$run_log" | sed 's/^/    /' >&2
    continue
  fi

  if (( run_rc == 0 )); then
    parse_test_log "$name" "$run_log"
    if (( test_marker_ok )) && (( test_checks > 0 )); then
      passed=$((passed + 1))
      total_checks=$((total_checks + test_checks))
      if (( verbose )); then
        printf '  [run]   %-34s PASS (%d checks)\n' "$test" "$test_checks"
        printf '%s\n' "    --- $name output ---"
        sed -n '1,80p' "$run_log" | sed 's/^/    /'
      else
        printf '  [run]   %-34s PASS (%d checks)\n' "$test" "$test_checks"
      fi
    else
      failed=$((failed + 1))
      if (( test_marker_ok == 0 )); then
        failed_names+=("$name (no completion marker)")
        printf '  [run]   %-34s FAIL (no completion marker / FAIL line)\n' "$test"
      else
        failed_names+=("$name (no checks executed)")
        printf '  [run]   %-34s FAIL (no "ok" check lines - gutted test?)\n' "$test"
      fi
      printf '    a passing test must print "ok    <what>" per check and a final "PASS (0 failures)"\n' >&2
      printf '    (or "test_<name>: all checks passed"); see TOOL-12 in the header of this script\n' >&2
      sed -n '1,80p' "$run_log" | sed 's/^/    /' >&2
    fi
  else
    failed=$((failed + 1))
    failed_names+=("$name (exit $run_rc)")
    printf '  [run]   %-34s FAIL (exit %d)\n' "$test" "$run_rc"
    sed -n '1,80p' "$run_log" | sed 's/^/    /' >&2
  fi
done

printf '\n'
if (( total_warnings > 0 )); then
  vcs_warn "compiler diagnostics: $total_warnings warning(s) in ${#warning_tests[@]} test build(s): ${warning_tests[*]}"
else
  vcs_info "compiler diagnostics: 0 warnings in ${#selected[@]} test build(s)"
fi
if (( failed == 0 )); then
  if (( no_run )); then
    # Compiling is not running: say so, or a CI log would read like a pass.
    vcs_info "tests: $passed/$((passed + failed)) compiled (--no-run: not executed, completion markers not checked)"
  else
    vcs_info "tests: $passed/$((passed + failed)) passed ($total_checks check(s) executed)"
  fi
  exit 0
fi
vcs_info "tests: $passed/$((passed + failed)) passed, $failed failed"
for n in "${failed_names[@]}"; do
  printf '  FAILED: %s\n' "$n" >&2
done
exit 1
