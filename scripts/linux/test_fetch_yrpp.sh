#!/usr/bin/env bash
# Regression tests for scripts/linux/fetch_yrpp.sh.
#
#   scripts/linux/test_fetch_yrpp.sh
#
# Covers D-09 (the script used to delete external/YRpp when it was asked to
# install the installed tree onto itself) and S-01 (--commit used to override
# the HEAD of a git checkout, so a stamp for the pinned revision could be written
# for arbitrary content):
#
#   A) installed + --commit             -> no-op, exit 0, tree byte-identical
#   B) installed + no arguments         -> no-op, exit 0, tree byte-identical
#   C) git checkout source at the pin   -> installs, check_yrpp.py OK, no .git kept
#   D) git source HEAD != --commit      -> refused, installed tree unchanged
#   E) --from external/YRpp (self-copy) -> refused, installed tree unchanged
#   F) source without .git + --commit   -> installs, warns that content is unverified
#   G) installed but stale stamp, no args -> refused (still no self-destruct)
#   H) external/YRpp is a git checkout, no stamp, no args -> refused
#
# Everything happens in one-time mktemp -d copies.  The repository itself is only
# read: its installed external/YRpp is mirrored for cases A/B/E/G and its stamps
# are checked at the end to prove the tests touched nothing.  No Wine and no
# MSVC are involved - pure shell and git - so this runs in a few seconds.
#
# Exit code 0 = every case passed, 1 = at least one failure.

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCS_ROOT="$(cd "$VCS_LINUX_DIR/../.." && pwd)"
FETCH="$VCS_LINUX_DIR/fetch_yrpp.sh"
CHECK="$VCS_LINUX_DIR/check_yrpp.py"
REAL_DEST="$VCS_ROOT/external/YRpp"
REAL_STAMP="$VCS_ROOT/external/YRpp.commit"

[[ -f $FETCH && -f $CHECK ]] || { printf 'ERROR: missing %s or %s\n' "$FETCH" "$CHECK" >&2; exit 1; }
[[ -f $REAL_DEST/YRPP.h && -f $REAL_STAMP ]] || {
  printf 'ERROR: %s is not installed; run scripts/linux/fetch_yrpp.sh first\n' "$REAL_DEST" >&2
  exit 1
}

work="$(mktemp -d "${TMPDIR:-/tmp}/vcs-test-fetch-yrpp.XXXXXX")"
trap 'rm -rf "$work"' EXIT

passed=0
failed=0
pass() { passed=$((passed + 1)); printf 'PASS: %s\n' "$*"; }
fail() { failed=$((failed + 1)); printf 'FAIL: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

count_files()  { find "$1" -type f 2>/dev/null | wc -l | tr -d ' '; }
fingerprint()  { find "$1" -type f -exec md5sum {} + 2>/dev/null | LC_ALL=C sort | md5sum | cut -d' ' -f1; }
realpin()      { sed -n 's/^#define[[:space:]]\{1,\}VC_YRPP_PIN[[:space:]]\{1,\}"\([0-9a-fA-F]*\)".*/\1/p' "$1"; }
stamp_of()     { tr -d '[:space:]' < "$1/external/YRpp.commit" 2>/dev/null || true; }

# Runs the copy of fetch_yrpp.sh inside <tree>; stores output in <out> and the
# exit code in $fetch_rc.  "set +e" is required because most failures below are
# the expected outcome.
run_fetch() {
  local tree="$1" out="$2"
  shift 2
  set +e
  ( cd "$tree" && bash scripts/linux/fetch_yrpp.sh "$@" ) >"$out" 2>&1
  fetch_rc=$?
  set -e
}

check_rc() { # label expected
  if [[ $fetch_rc == "$2" ]]; then
    pass "$1: exit code $fetch_rc"
  else
    fail "$1: exit code $fetch_rc (expected $2)"
    sed 's/^/    | /' "$fetch_out" >&2
  fi
}

check_refused() { # label
  if (( fetch_rc != 0 )); then
    pass "$1: refused (exit code $fetch_rc)"
  else
    fail "$1: was expected to be refused but exited 0"
    sed 's/^/    | /' "$fetch_out" >&2
  fi
}

check_contains() { # label needle
  if grep -qF -- "$2" "$fetch_out"; then
    pass "$1: output mentions '$2'"
  else
    fail "$1: output does not mention '$2'"
    sed 's/^/    | /' "$fetch_out" >&2
  fi
}

check_unchanged() { # label dir expected-fingerprint
  local now
  now="$(fingerprint "$2")"
  if [[ $now == "$3" ]]; then
    pass "$1: tree unchanged"
  else
    fail "$1: tree changed ($3 -> $now)"
  fi
}

check_installed() { # label tree expected-commit
  local tree="$2"
  if [[ -f $tree/external/YRpp/YRPP.h ]]; then
    pass "$1: external/YRpp installed"
  else
    fail "$1: external/YRpp is missing or incomplete"
  fi
  if [[ -e $tree/external/YRpp/.git ]]; then
    fail "$1: copied .git should have been dropped"
  else
    pass "$1: upstream .git dropped"
  fi
  if [[ "$(stamp_of "$tree")" == "$3" ]]; then
    pass "$1: stamp records $3"
  else
    fail "$1: stamp is '$(stamp_of "$tree")' (expected $3)"
  fi
  if python3 "$tree/scripts/linux/check_yrpp.py" >/dev/null 2>&1; then
    pass "$1: check_yrpp.py OK"
  else
    fail "$1: check_yrpp.py rejected the installed tree"
  fi
  check_no_staging_leftovers "$1" "$tree"
}

check_no_staging_leftovers() { # label tree
  local leftovers
  leftovers="$(find "$2/external" -maxdepth 1 \( -name '.YRpp.new.*' -o -name '.YRpp.old.*' -o -name 'YRpp.commit.new.*' \) -print 2>/dev/null)"
  if [[ -z $leftovers ]]; then
    pass "$1: no staging leftovers"
  else
    fail "$1: staging leftovers remain: $leftovers"
  fi
}

make_mirror_tree() { # tree
  local tree="$1"
  mkdir -p "$tree/scripts/linux" "$tree/src/Core" "$tree/external"
  cp "$FETCH" "$tree/scripts/linux/fetch_yrpp.sh"
  cp "$CHECK" "$tree/scripts/linux/check_yrpp.py"
  cp "$VCS_ROOT/src/Core/EnginePin.h" "$tree/src/Core/EnginePin.h"
  cp -a "$REAL_DEST" "$tree/external/YRpp"
  cp "$REAL_STAMP" "$tree/external/YRpp.commit"
  # What fetch_yrpp.sh installs: headers plus the commit stamp, no .git.
  rm -rf "$tree/external/YRpp/.git"
}

make_fixture_tree() { # tree pin
  local tree="$1" pin="$2"
  mkdir -p "$tree/scripts/linux" "$tree/src/Core" "$tree/external"
  cp "$FETCH" "$tree/scripts/linux/fetch_yrpp.sh"
  cp "$CHECK" "$tree/scripts/linux/check_yrpp.py"
  {
    printf '// Synthetic pin used by scripts/linux/test_fetch_yrpp.sh.\n'
    printf '#define VC_YRPP_PIN "%s"\n' "$pin"
    printf '#define VC_YRPP_PIN_SHORT "%.8s"\n' "$pin"
  } > "$tree/src/Core/EnginePin.h"
}

fixture_commit() { # repo message
  git -C "$1" -c user.name='VentureCore tests' -c user.email='vc-tests@example.invalid' \
    -c commit.gpgsign=false commit -q -m "$2"
}

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
real_pin="$(realpin "$VCS_ROOT/src/Core/EnginePin.h")"
[[ -n $real_pin ]] || die "cannot read VC_YRPP_PIN from src/Core/EnginePin.h"
real_files_start="$(count_files "$REAL_DEST")"
real_fp_start="$(fingerprint "$REAL_DEST")"
real_stamp_start="$(tr -d '[:space:]' < "$REAL_STAMP")"

printf '== mirror of the installed tree (pin %s)\n' "$real_pin"
T1="$work/mirror"
make_mirror_tree "$T1"
[[ "$(realpin "$T1/src/Core/EnginePin.h")" == "$real_pin" ]] || die "mirror tree pin mismatch"
mirror_files="$(count_files "$T1/external/YRpp")"
[[ $mirror_files == "$real_files_start" ]] \
  && pass "mirror: copied all $mirror_files installed files" \
  || fail "mirror: copied $mirror_files files (installed tree has $real_files_start)"

# ---------------------------------------------------------------------------
# A) installed + --commit: no source named, already at the pin -> no-op
# ---------------------------------------------------------------------------
fetch_out="$work/a.out"
fp="$(fingerprint "$T1/external")"
run_fetch "$T1" "$fetch_out" --commit "$real_pin"
check_rc "A installed + --commit" 0
check_contains "A installed + --commit" "already installed at $real_pin; nothing to do"
check_unchanged "A installed + --commit" "$T1/external" "$fp"
[[ "$(count_files "$T1/external/YRpp")" == "$mirror_files" ]] \
  && pass "A installed + --commit: still $mirror_files files" \
  || fail "A installed + --commit: file count changed"
[[ "$(stamp_of "$T1")" == "$real_pin" ]] \
  && pass "A installed + --commit: stamp untouched" \
  || fail "A installed + --commit: stamp changed"

# ---------------------------------------------------------------------------
# B) installed + no arguments: must not self-destruct either
# ---------------------------------------------------------------------------
fetch_out="$work/b.out"
fp="$(fingerprint "$T1/external")"
run_fetch "$T1" "$fetch_out"
check_rc "B installed + no arguments" 0
check_contains "B installed + no arguments" "already installed at $real_pin; nothing to do"
check_unchanged "B installed + no arguments" "$T1/external" "$fp"
[[ -f $T1/external/YRpp/YRPP.h ]] \
  && pass "B installed + no arguments: external/YRpp still there" \
  || fail "B installed + no arguments: external/YRpp was deleted"

# ---------------------------------------------------------------------------
# Fixture: a real git checkout whose HEAD is the (synthetic) pin
# ---------------------------------------------------------------------------
REPO="$work/yrpp-src"
mkdir -p "$REPO"
printf '#pragma once\n// fixture YRPP.h for test_fetch_yrpp.sh\n' > "$REPO/YRPP.h"
printf '// fixture StaticInits.cpp\n' > "$REPO/StaticInits.cpp"
git init -q "$REPO"
git -C "$REPO" add -A
fixture_commit "$REPO" initial
C1="$(git -C "$REPO" rev-parse HEAD)"

# Same content without .git: the only source fetch_yrpp.sh may take on trust.
NOGIT="$work/yrpp-nogit"
mkdir -p "$NOGIT"
cp "$REPO/YRPP.h" "$REPO/StaticInits.cpp" "$NOGIT/"

# ---------------------------------------------------------------------------
# C) --from <git checkout at the pin> --commit <pin>: installs
# ---------------------------------------------------------------------------
T2="$work/fixture"
make_fixture_tree "$T2" "$C1"
fetch_out="$work/c.out"
run_fetch "$T2" "$fetch_out" --from "$REPO" --commit "$C1"
check_rc "C git source at pin" 0
check_installed "C git source at pin" "$T2" "$C1"
cmp -s "$REPO/YRPP.h" "$T2/external/YRpp/YRPP.h" \
  && pass "C git source at pin: content copied" \
  || fail "C git source at pin: copied content differs"

# ---------------------------------------------------------------------------
# H) external/YRpp is a git checkout (no stamp) + no args: refuse, no deletion
#    (the D-09 variant: the implicit source search finds the installed tree)
# ---------------------------------------------------------------------------
T4="$work/git-installed"
make_fixture_tree "$T4" "$C1"
cp -a "$REPO" "$T4/external/YRpp"
fp="$(fingerprint "$T4/external")"
fetch_out="$work/h.out"
run_fetch "$T4" "$fetch_out"
check_refused "H installed as git checkout + no args"
check_contains "H installed as git checkout + no args" "refusing to copy"
check_unchanged "H installed as git checkout + no args" "$T4/external" "$fp"
[[ -f $T4/external/YRpp/YRPP.h ]] \
  && pass "H installed as git checkout + no args: external/YRpp still there" \
  || fail "H installed as git checkout + no args: external/YRpp was deleted"

# ---------------------------------------------------------------------------
# D) git source HEAD != --commit: refuse (S-01 pin gate must not be bypassed)
# ---------------------------------------------------------------------------
printf '// second commit\n' > "$REPO/extra.txt"
git -C "$REPO" add -A
fixture_commit "$REPO" second
C2="$(git -C "$REPO" rev-parse HEAD)"
[[ $C2 != "$C1" ]] || die "fixture repository did not get a second commit"
fp="$(fingerprint "$T2/external")"
fetch_out="$work/d.out"
run_fetch "$T2" "$fetch_out" --from "$REPO" --commit "$C1"
check_refused "D git HEAD $C2 vs --commit $C1"
check_contains "D git HEAD $C2 vs --commit $C1" "never overrides"
check_unchanged "D git HEAD $C2 vs --commit $C1" "$T2/external" "$fp"
[[ "$(stamp_of "$T2")" == "$C1" ]] \
  && pass "D git HEAD != --commit: installed stamp unchanged" \
  || fail "D git HEAD != --commit: installed stamp changed"

# ---------------------------------------------------------------------------
# E) --from external/YRpp (explicit self-copy): refuse even when installed at pin
# ---------------------------------------------------------------------------
fetch_out="$work/e.out"
fp="$(fingerprint "$T1/external")"
run_fetch "$T1" "$fetch_out" --from "$T1/external/YRpp"
check_refused "E explicit --from external/YRpp"
check_contains "E explicit --from external/YRpp" "refusing to copy"
check_unchanged "E explicit --from external/YRpp" "$T1/external" "$fp"
[[ "$(count_files "$T1/external/YRpp")" == "$mirror_files" ]] \
  && pass "E explicit --from external/YRpp: still $mirror_files files" \
  || fail "E explicit --from external/YRpp: file count changed"

# ---------------------------------------------------------------------------
# F) source without .git + --commit: installs, but says the content is unverified
# ---------------------------------------------------------------------------
rm -rf "$T2/external/YRpp" "$T2/external/YRpp.commit"
fetch_out="$work/f.out"
run_fetch "$T2" "$fetch_out" --from "$NOGIT" --commit "$C1"
check_rc "F no-.git source + --commit" 0
check_contains "F no-.git source + --commit" "cannot be verified"
check_contains "F no-.git source + --commit" "only the claimed commit"
check_installed "F no-.git source + --commit" "$T2" "$C1"

# ---------------------------------------------------------------------------
# G) installed but stamped with a different revision + no args: refuse, keep tree
# ---------------------------------------------------------------------------
T3="$work/stale-stamp"
make_mirror_tree "$T3"
printf 'deadbeefdeadbeefdeadbeefdeadbeefdeadbeef\n' > "$T3/external/YRpp.commit"
fp="$(fingerprint "$T3/external")"
fetch_out="$work/g.out"
run_fetch "$T3" "$fetch_out"
check_refused "G stale stamp + no args"
check_contains "G stale stamp + no args" "refusing to copy"
check_unchanged "G stale stamp + no args" "$T3/external" "$fp"
[[ "$(count_files "$T3/external/YRpp")" == "$mirror_files" ]] \
  && pass "G stale stamp + no args: still $mirror_files files" \
  || fail "G stale stamp + no args: file count changed"

# ---------------------------------------------------------------------------
# The real tree must be untouched: no staging directories, same stamp and files
# ---------------------------------------------------------------------------
printf '\n== integrity of the real installed tree\n'
real_pin_now="$(realpin "$VCS_ROOT/src/Core/EnginePin.h")"
[[ $real_pin_now == "$real_pin" ]] \
  && pass "real tree: pin unchanged" \
  || fail "real tree: pin changed"
[[ "$(tr -d '[:space:]' < "$REAL_STAMP")" == "$real_stamp_start" ]] \
  && pass "real tree: external/YRpp.commit unchanged" \
  || fail "real tree: external/YRpp.commit changed"
fp="$(fingerprint "$REAL_DEST")"
[[ $fp == "$real_fp_start" ]] \
  && pass "real tree: external/YRpp unchanged" \
  || fail "real tree: external/YRpp changed"
[[ "$(count_files "$REAL_DEST")" == "$real_files_start" ]] \
  && pass "real tree: external/YRpp still $real_files_start files" \
  || fail "real tree: external/YRpp file count changed"
if python3 "$CHECK"; then
  pass "real tree: check_yrpp.py OK"
else
  fail "real tree: check_yrpp.py failed"
fi
leftovers="$(find "$VCS_ROOT/external" -maxdepth 1 \( -name '.YRpp.new.*' -o -name '.YRpp.old.*' -o -name 'YRpp.commit.new.*' \) -print 2>/dev/null)"
if [[ -z $leftovers ]]; then
  pass "real tree: no staging leftovers"
else
  fail "real tree: staging leftovers remain: $leftovers"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
printf '\n'
if (( failed == 0 )); then
  printf 'test_fetch_yrpp: all %d check(s) PASS\n' "$passed"
  exit 0
fi
printf 'test_fetch_yrpp: %d check(s) FAILED (%d passed)\n' "$failed" "$passed" >&2
exit 1
