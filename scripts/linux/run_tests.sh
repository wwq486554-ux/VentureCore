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
# Options:
#   --list        list the discovered tests and exit
#   --verbose     echo the compiler command line and always print the output
#   --no-run      only compile, do not execute
#   -h, --help
#
# The toolchain lock from common.sh is held while compiling, so this can run
# next to a build without corrupting the shared Wine prefix - but it does not
# make a concurrent *build* safe against a partially edited tree; do not edit
# sources while testing.
#
# Notes:
#   * `main` is declared `int __cdecl main(void)` because the platform is built
#     with /Gz (stdcall default), exactly like the load/ABI probes;
#   * the tests include headers only from src/ and include/ (already on
#     INCLUDE), so they work without YRpp or gamemd.exe.

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$VCS_LINUX_DIR/common.sh"

usage() { awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"; }

verbose=0
no_run=0
list_only=0
filter=""

while (( $# )); do
  case "$1" in
    --list)     list_only=1 ;;
    --verbose)  verbose=1 ;;
    --no-run)   no_run=1 ;;
    -h|--help)  usage; exit 0 ;;
    -*)         vcs_die "unknown argument: $1 (try --help)" ;;
    *)          filter="$1" ;;
  esac
  shift
done

tests_dir="$VCS_ROOT/tests"
[[ -d $tests_dir ]] || { vcs_warn "no tests/ directory in $VCS_ROOT"; exit 0; }

mapfile -t all_tests < <(find "$tests_dir" -maxdepth 1 -type f -name 'test_*.cpp' -printf '%f\n' | sort)
(( ${#all_tests[@]} > 0 )) || { vcs_warn "tests/ contains no test_*.cpp"; exit 0; }

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
mkdir -p "$work_dir"

cl_flags=(
  /nologo /W4 /WX- /MT /EHsc- /Gz /std:c++20
  /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000
  /wd4100 /wd4201 /wd4458 /wd4819
)

failed=0
passed=0
declare -a failed_names=()
declare -a warning_tests=()
total_warnings=0

vcs_lock_toolchain

# Tests compile the platform sources directly, so the generated git stamp header
# that src/Core/PlatformVersion.h includes has to exist and be on INCLUDE.
gen_dir="$VCS_ROOT/Release/IntDir/Generated"
vcs_generate_git_header "$gen_dir"
vcs_wine_env "$gen_dir"

for test in "${selected[@]}"; do
  name="${test%.cpp}"
  src="$tests_dir/$test"
  exe="$work_dir/$name.exe"
  log="$work_dir/$name.build.log"

  # TEST_DEPS: one comment line with repo-relative source paths.
  deps_line="$(grep -m1 '^// *TEST_DEPS:' "$src" | sed -e 's|^// *TEST_DEPS: *||' -e 's|\r$||')"
  sources=("$src")
  for dep in $deps_line; do
    [[ -f "$VCS_ROOT/$dep" ]] || vcs_die "$test: TEST_DEPS names a missing source: $dep"
    sources+=("$VCS_ROOT/$dep")
  done
  win_sources=()
  for s in "${sources[@]}"; do win_sources+=("$(vcs_w "$s")"); done

  if (( verbose )); then
    vcs_info "CC $test${deps_line:+ (+ $deps_line)}"
  else
    printf '  [build] %-34s' "$test"
  fi

  set +e
  ( cd "$work_dir" && wine "$VCS_VC_BIN/cl.exe" "${cl_flags[@]}" "${win_sources[@]}" \
      "/Fe$(vcs_w "$exe")" /link /SUBSYSTEM:CONSOLE /MACHINE:X86 \
      dbghelp.lib onecore.lib ) >"$log" 2>&1
  compile_rc=$?
  set -e

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
  ( cd "$work_dir" && wine "$(vcs_w "$exe")" ) >"$run_log" 2>&1
  run_rc=$?
  set -e

  if (( run_rc == 0 )); then
    passed=$((passed + 1))
    if (( verbose )); then
      printf '%s\n' "    --- $name output ---"
      sed -n '1,80p' "$run_log" | sed 's/^/    /'
    else
      printf '  [run]   %-34s PASS\n' "$test"
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
  vcs_info "tests: $passed/$((passed + failed)) passed"
  exit 0
fi
vcs_info "tests: $passed/$((passed + failed)) passed, $failed failed"
for n in "${failed_names[@]}"; do
  printf '  FAILED: %s\n' "$n" >&2
done
exit 1
