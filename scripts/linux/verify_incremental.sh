#!/usr/bin/env bash
# Prove that the incremental build invalidates every real build input (D-10/D-11).
#
#   scripts/linux/verify_incremental.sh [--jobs N] [--logdir DIR] [--keep]
#
# The working tree is copied into a one-time mktemp -d directory and real
# MSVC-under-Wine builds are run there, so the repository is never modified.
# Each step asserts on the build log:
#
#   1. fresh build: every translation unit compiles, no warnings;
#   2. no-change rebuild: "0 to compile" + "up to date - nothing to link";
#   3. edit include/platform/Feature.h on a clean tree: TUs recompile + relink;
#   4. edit it again on a dirty tree: ALL TUs still recompile.  This is the
#      D-10 case - the generated git header no longer changes, so only the
#      include/ header stamp can trigger the rebuild (before the fix: 0 TUs);
#   5. edit src/Core/Platform.cpp: exactly that TU recompiles + relinks;
#   6. change the linker command line (/TLBID:1 -> /TLBID:2): 0 TUs compile,
#      the DLL relinks (D-11);
#   7. edit VentureCore.props: TUs recompile + relink (project files are
#      build inputs).
#
# The copy gets a private one-commit git repository instead of the source's
# .git (which may be a worktree pointer into another repository), so clean-tree
# vs dirty-tree behaviour of the generated VentureCore.Git.h is realistic without
# ever touching the original repository.  VCS_WINEPREFIX is inherited: export
# the same prefix a normal build uses (required when building from a git
# worktree).  The child builds take the shared toolchain flock, so this script
# serialises with any other Wine job.
#
# Exit code 0 = every check passed, 1 = at least one failure.
# Build logs are written to <logdir> (default: Release/IntDir/verify-incremental).

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCS_ROOT="$(cd "$VCS_LINUX_DIR/../.." && pwd)"

info() { printf '[verify-incremental] %s\n' "$*" >&2; }
die()  { printf '[verify-incremental] error: %s\n' "$*" >&2; exit 1; }
pass() { passed=$((passed + 1)); printf 'PASS: %s\n' "$*"; }
fail() { failed=$((failed + 1)); printf 'FAIL: %s\n' "$*" >&2; }
section() { printf '\n== %s\n' "$*"; }
usage() { awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "${BASH_SOURCE[0]}"; }

jobs="$(nproc 2>/dev/null || echo 4)"
logdir=""
keep=0
while (( $# )); do
  case "$1" in
    -j|--jobs) jobs="${2:?--jobs needs a value}"; shift ;;
    --logdir)  logdir="${2:?--logdir needs a directory}"; shift ;;
    --keep)    keep=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
  shift
done
[[ $jobs =~ ^[0-9]+$ && $jobs -gt 0 ]] || die "--jobs needs a positive integer"
logdir="${logdir:-$VCS_ROOT/Release/IntDir/verify-incremental}"
mkdir -p "$logdir"

command -v wine >/dev/null 2>&1 || die "wine is not installed"
[[ -f $VCS_ROOT/external/YRpp/YRPP.h ]] || die \
  "external/YRpp is not installed - run scripts/linux/fetch_yrpp.sh first"

passed=0
failed=0
copy="$(mktemp -d "${TMPDIR:-/tmp}/vcs-verify-incremental.XXXXXX")"
cleanup() {
  if (( keep )); then
    info "kept the build copy at $copy"
  elif [[ -d $copy ]]; then
    rm -rf "$copy"
  fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# One-time copy of the working tree + a private snapshot repository
# ---------------------------------------------------------------------------
info "copying $VCS_ROOT -> $copy"
cp -a "$VCS_ROOT/." "$copy/"
rm -rf "$copy/Release" "$copy/Debug"
# The source may be a git worktree (.git is a file pointing into another
# repository); never carry that link into the copy.  A private single-commit
# repository gives the same clean-tree/dirty-tree behaviour for the generated
# git header without any commit touching the original repository.
rm -rf "$copy/.git"
git init -q "$copy"
git -C "$copy" add -A
git -C "$copy" -c user.name='VentureCore verify-incremental' \
  -c user.email='vc-verify@example.invalid' -c commit.gpgsign=false \
  commit -q -m "snapshot for scripts/linux/verify_incremental.sh"
[[ -z "$(git -C "$copy" status --porcelain)" ]] || die "the copied tree is not clean"
info "copied and snapshotted ($(git -C "$copy" rev-parse --short HEAD))"

build_log=""
build() { # label log-name
  local label="$1" log="$logdir/$2.log"
  info "build: $label"
  set +e
  ( cd "$copy" && scripts/linux/build_linux.sh Release --build-type RELEASE -j "$jobs" ) \
    >"$log" 2>&1
  build_rc=$?
  set -e
  build_log="$log"
  if (( build_rc != 0 )); then
    fail "$label: build failed (exit $build_rc)"
    tail -n 40 "$log" >&2
    summary
  fi
  pass "$label: build succeeded"
}

build_total()   { sed -n 's/.*sources *: \([0-9][0-9]*\) total, .*/\1/p' "$build_log" | tail -n 1; }
build_pending() { sed -n 's/.*sources *: [0-9][0-9]* total, \([0-9][0-9]*\) to compile.*/\1/p' "$build_log" | tail -n 1; }

assert_equals() { # label actual expected
  if [[ $2 == "$3" ]]; then
    pass "$1: $2"
  else
    fail "$1: got '$2', expected '$3'"
  fi
}
assert_contains() { # label needle
  if grep -qF -- "$2" "$build_log"; then
    pass "$1: output contains '$2'"
  else
    fail "$1: output does not contain '$2'"
    tail -n 30 "$build_log" >&2
  fi
}
assert_not_contains() { # label needle
  if grep -qF -- "$2" "$build_log"; then
    fail "$1: output unexpectedly contains '$2'"
  else
    pass "$1: output does not contain '$2'"
  fi
}
assert_no_warnings() { # label
  if grep -qE 'warning C[0-9]+' "$build_log"; then
    fail "$1: compiler warnings found"
    grep -E 'warning C[0-9]+' "$build_log" | head -n 10 >&2
  else
    pass "$1: no compiler warnings"
  fi
}
assert_compile_count() { # label expected
  local got
  got="$(build_pending)"
  if [[ $got == "$2" ]]; then
    pass "$1: $got translation unit(s) to compile (expected $2)"
  else
    fail "$1: $got translation unit(s) to compile (expected $2)"
  fi
}
assert_full_rebuild() { # label
  local total pending
  total="$(build_total)"
  pending="$(build_pending)"
  if [[ -n $total && $pending == "$total" ]] && (( total > 0 )); then
    pass "$1: all $total translation unit(s) recompiled"
  else
    fail "$1: $pending/$total translation unit(s) recompiled (expected all)"
  fi
}
assert_clean() { # label
  if [[ -z "$(git -C "$copy" status --porcelain)" ]]; then
    pass "$1: working tree is clean"
  else
    fail "$1: working tree is dirty"
    git -C "$copy" status --porcelain >&2
  fi
}
append_line() { # file text - keeps CRLF files CRLF
  printf '\r\n%s\r\n' "$2" >> "$1"
}

summary() {
  printf '\n'
  if (( failed == 0 )); then
    printf '[verify-incremental] all %d check(s) PASS\n' "$passed"
    printf '[verify-incremental] build logs: %s\n' "$logdir"
    exit 0
  fi
  printf '[verify-incremental] %d check(s) FAILED (%d passed)\n' "$failed" "$passed" >&2
  printf '[verify-incremental] build logs: %s\n' "$logdir" >&2
  exit 1
}

# ---------------------------------------------------------------------------
# 1. Fresh build
# ---------------------------------------------------------------------------
section "1. fresh build (no Release/ in the copy)"
build "fresh build" "01-fresh-build"
assert_contains "fresh build" "[linux-build] output:"
assert_no_warnings "fresh build"
assert_full_rebuild "fresh build"

# ---------------------------------------------------------------------------
# 2. No changes: nothing compiles, nothing links
# ---------------------------------------------------------------------------
section "2. rebuild without changes"
build "no-change rebuild" "02-no-change"
assert_compile_count "no-change rebuild" 0
assert_contains "no-change rebuild" "up to date - nothing to link"

# ---------------------------------------------------------------------------
# 3. Clean tree: a real content edit in include/ must recompile
# ---------------------------------------------------------------------------
section "3. edit include/platform/Feature.h on a clean tree"
assert_clean "clean-tree include/ edit"
append_line "$copy/include/platform/Feature.h" "// verify_incremental marker (clean tree)"
build "include/ edit (clean tree)" "03-include-edit-clean-tree"
assert_full_rebuild "include/ edit (clean tree)"
assert_contains "include/ edit (clean tree)" "linking"

# ---------------------------------------------------------------------------
# 4. Dirty tree: the D-10 case - the git header no longer changes, so only the
#    include/ header stamp can force the rebuild
# ---------------------------------------------------------------------------
section "4. edit include/platform/Feature.h again on a dirty tree (D-10)"
git_header="$copy/Release/IntDir/Generated/VentureCore.Git.h"
[[ -f $git_header ]] || die "generated git header missing: $git_header"
git_header_before="$(md5sum "$git_header" | cut -d' ' -f1)"
append_line "$copy/include/platform/Feature.h" "// verify_incremental marker (dirty tree)"
build "include/ edit (dirty tree)" "04-include-edit-dirty-tree"
git_header_after="$(md5sum "$git_header" | cut -d' ' -f1)"
assert_equals "dirty-tree include/ edit: generated git header unchanged" \
  "$git_header_after" "$git_header_before"
assert_full_rebuild "include/ edit (dirty tree)"
assert_contains "include/ edit (dirty tree)" "linking"

# ---------------------------------------------------------------------------
# 5. Source edit: exactly one translation unit recompiles, then relink
# ---------------------------------------------------------------------------
section "5. edit src/Core/Platform.cpp"
append_line "$copy/src/Core/Platform.cpp" "// verify_incremental marker (source)"
build "Platform.cpp edit" "05-source-edit"
assert_compile_count "Platform.cpp edit" 1
assert_contains "Platform.cpp edit" "CC src/Core/Platform.cpp"
assert_contains "Platform.cpp edit" "linking"

# ---------------------------------------------------------------------------
# 6. Linker command line change: no compilation, but a relink (D-11)
# ---------------------------------------------------------------------------
section "6. change the linker command line (/TLBID:1 -> /TLBID:2)"
sed -i 's#/TLBID:1#/TLBID:2#' "$copy/scripts/linux/build_linux.sh"
grep -qF '/TLBID:2' "$copy/scripts/linux/build_linux.sh" \
  || die "could not patch /TLBID in the copy's build_linux.sh"
build "linker-flags change" "06-linker-flags"
assert_compile_count "linker-flags change" 0
assert_contains "linker-flags change" "linker inputs changed - relink"
assert_contains "linker-flags change" "linking"

# ---------------------------------------------------------------------------
# 7. Project file edit: recompiles (source list/include path input) and relinks
# ---------------------------------------------------------------------------
section "7. edit VentureCore.props"
python3 - "$copy/VentureCore.props" <<'PY'
import sys

path = sys.argv[1]
data = open(path, "rb").read()
marker = b"  <!-- verify_incremental marker -->\r\n"
index = data.rfind(b"</Project>")
if index == -1:
    raise SystemExit("no </Project> in VentureCore.props")
open(path, "wb").write(data[:index] + marker + data[index:])
PY
build "props edit" "07-props-edit"
assert_full_rebuild "props edit"
assert_contains "props edit" "linking"

summary
