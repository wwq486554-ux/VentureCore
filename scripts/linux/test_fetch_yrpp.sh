#!/usr/bin/env bash
# Regression tests for scripts/linux/fetch_yrpp.sh.
#
#   scripts/linux/test_fetch_yrpp.sh
#
# Covers the pin gates of the 2.1 review (TOOL-09) together with the two earlier
# defects this script was written for:
#
#   * D-09: asking fetch_yrpp.sh to install the installed tree onto itself used
#     to delete external/YRpp;
#   * S-01: --commit used to override the HEAD of a git checkout, so a stamp for
#     the pinned revision could be written for arbitrary content;
#   * TOOL-09: the revision alone is not a content gate.  check_yrpp.py now
#     accepts only the **full 40-hex** commit id (a short stamp that prefixes the
#     pin used to pass the old bidirectional prefix match), refuses a dirty git
#     checkout, and verifies every file against the tracked sha256 manifest
#     external/YRpp.sha256.  fetch_yrpp.sh verifies the *staged* copy against
#     that manifest before it replaces the installed tree, so a source without a
#     .git can no longer install unverified content under a claimed commit.
#
# Cases:
#   A) installed + --commit              -> no-op, exit 0, tree byte-identical
#   B) installed + no arguments          -> no-op, exit 0, tree byte-identical
#   C) git source at the pin + --write-manifest
#                                        -> installs, generates the manifest
#   C2) git source at the pin, manifest matches
#                                        -> installs (staged content verified)
#   D) git source HEAD != --commit       -> refused, installed tree unchanged
#   E) --from external/YRpp (self-copy)  -> refused, installed tree unchanged
#   F) source without .git + --commit    -> installs, content verified against
#                                           the manifest (revision is claimed)
#   G) installed but stale stamp, no args-> refused (still no self-destruct)
#   H) external/YRpp is a git checkout, no stamp, no args -> refused
#   I) tampered installed content        -> check_yrpp.py fails, fetch is refused
#   J) short commit stamp                -> refused (full 40-hex id required)
#   K) missing manifest                  -> refused; --write-manifest regenerates
#                                           it identically to the tracked one
#   L) short pin in EnginePin.h          -> refused (full 40-hex id required)
#   P) mismatched VC_YRPP_PIN_SHORT      -> refused (the short id is display-only)
#   M) dirty installed git checkout      -> check_yrpp.py fails
#   N) dirty source git checkout         -> refused
#   O) stale manifest after a pin bump   -> staged content is refused *before*
#                                           the swap; --write-manifest then
#                                           installs it and updates the manifest
#
# Everything happens in one-time mktemp -d copies.  The repository itself is only
# read: its installed external/YRpp is mirrored for cases A/B/E/G/I/J/K and its
# stamps/manifest are checked at the end to prove the tests touched nothing.  No
# Wine and no MSVC are involved - pure shell, python and git - so this runs in a
# few seconds.
#
# Exit code 0 = every case passed, 1 = at least one failure.

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCS_ROOT="$(cd "$VCS_LINUX_DIR/../.." && pwd)"
FETCH="$VCS_LINUX_DIR/fetch_yrpp.sh"
CHECK="$VCS_LINUX_DIR/check_yrpp.py"
REAL_DEST="$VCS_ROOT/external/YRpp"
REAL_STAMP="$VCS_ROOT/external/YRpp.commit"
REAL_MANIFEST="$VCS_ROOT/external/YRpp.sha256"

[[ -f $FETCH && -f $CHECK ]] || { printf 'ERROR: missing %s or %s\n' "$FETCH" "$CHECK" >&2; exit 1; }
[[ -f $REAL_DEST/YRPP.h && -f $REAL_STAMP && -f $REAL_MANIFEST ]] || {
  printf 'ERROR: %s is not installed (or external/YRpp.sha256 is missing); run scripts/linux/fetch_yrpp.sh first\n' "$REAL_DEST" >&2
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
normalize_manifest() { tr -d '\r' < "$1"; }

fetch_rc=0
fetch_out=""
check_exit=0
check_out=""

# Runs the copy of fetch_yrpp.sh inside <tree>; stores output in <out> and the
# exit code in fetch_rc.  "set +e" is required because most failures below are
# the expected outcome.
run_fetch() {
  local tree="$1" out="$2"
  shift 2
  fetch_out="$out"
  set +e
  ( cd "$tree" && bash scripts/linux/fetch_yrpp.sh "$@" ) >"$out" 2>&1
  fetch_rc=$?
  set -e
}

# Runs the copy of check_yrpp.py inside <tree>; stores output in <out> and the
# exit code in check_exit.
run_check() {
  local tree="$1" out="$2"
  shift 2
  check_out="$out"
  set +e
  python3 "$tree/scripts/linux/check_yrpp.py" "$@" >"$out" 2>&1
  check_exit=$?
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

check_check_ok() { # label
  if (( check_exit == 0 )); then
    pass "$1: check_yrpp.py accepts the tree"
  else
    fail "$1: check_yrpp.py rejected the tree"
    sed 's/^/    | /' "$check_out" >&2
  fi
}

check_check_fails() { # label needle
  if (( check_exit != 0 )); then
    pass "$1: check_yrpp.py rejected the tree (exit code $check_exit)"
  else
    fail "$1: check_yrpp.py accepted the tree but should not have"
  fi
  if grep -qF -- "$2" "$check_out"; then
    pass "$1: check_yrpp.py mentions '$2'"
  else
    fail "$1: check_yrpp.py does not mention '$2'"
    sed 's/^/    | /' "$check_out" >&2
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

check_same_manifest() { # label left right
  if diff -u --label "$2" --label "$3" <(normalize_manifest "$2") <(normalize_manifest "$3") >/dev/null 2>&1; then
    pass "$1: manifests are identical"
  else
    fail "$1: manifests differ"
    diff -u --label "$2" --label "$3" <(normalize_manifest "$2") <(normalize_manifest "$3") 2>&1 | sed -n '1,20p' | sed 's/^/    | /' >&2 || true
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
  if [[ -f $tree/external/YRpp.sha256 ]]; then
    pass "$1: external/YRpp.sha256 present"
  else
    fail "$1: external/YRpp.sha256 is missing"
  fi
  run_check "$tree" "$work/$(basename "$tree").check.out"
  check_check_ok "$1"
  check_no_staging_leftovers "$1" "$tree"
}

check_no_staging_leftovers() { # label tree
  local leftovers
  leftovers="$(find "$2/external" -maxdepth 1 \( -name '.YRpp.new.*' -o -name '.YRpp.old.*' -o -name 'YRpp.commit.new.*' -o -name '.YRpp.sha256.new.*' \) -print 2>/dev/null)"
  if [[ -z $leftovers ]]; then
    pass "$1: no staging leftovers"
  else
    fail "$1: staging leftovers remain: $leftovers"
  fi
}

write_fixture_pin() { # tree pin
  {
    printf '// Synthetic pin used by scripts/linux/test_fetch_yrpp.sh.\n'
    printf '#define VC_YRPP_PIN "%s"\n' "$2"
    printf '#define VC_YRPP_PIN_SHORT "%.8s"\n' "$2"
  } > "$1/src/Core/EnginePin.h"
}

make_mirror_tree() { # tree
  local tree="$1"
  mkdir -p "$tree/scripts/linux" "$tree/src/Core" "$tree/external"
  cp "$FETCH" "$tree/scripts/linux/fetch_yrpp.sh"
  cp "$CHECK" "$tree/scripts/linux/check_yrpp.py"
  cp "$VCS_ROOT/src/Core/EnginePin.h" "$tree/src/Core/EnginePin.h"
  cp -a "$REAL_DEST" "$tree/external/YRpp"
  cp "$REAL_STAMP" "$tree/external/YRpp.commit"
  cp "$REAL_MANIFEST" "$tree/external/YRpp.sha256"
  # What fetch_yrpp.sh installs: headers plus the commit stamp, no .git.
  rm -rf "$tree/external/YRpp/.git"
}

make_fixture_tree() { # tree pin
  local tree="$1"
  mkdir -p "$tree/scripts/linux" "$tree/src/Core" "$tree/external"
  cp "$FETCH" "$tree/scripts/linux/fetch_yrpp.sh"
  cp "$CHECK" "$tree/scripts/linux/check_yrpp.py"
  write_fixture_pin "$tree" "$2"
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
real_manifest_hash_start="$(md5sum "$REAL_MANIFEST" | cut -d' ' -f1)"

printf '== baseline: the real tree passes the pin/content gate\n'
run_check "$VCS_ROOT" "$work/baseline.check.out"
check_check_ok "baseline"

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
[[ "$(stamp_of "$T1")" == "$real_pin" ]] \
  && pass "A installed + --commit: stamp untouched" \
  || fail "A installed + --commit: stamp changed"
run_check "$T1" "$work/a.check.out"
check_check_ok "A installed + --commit"

# ---------------------------------------------------------------------------
# B) installed + no arguments: must not self-destruct either
# ---------------------------------------------------------------------------
fetch_out="$work/b.out"
fp="$(fingerprint "$T1/external")"
run_fetch "$T1" "$fetch_out"
check_rc "B installed + no arguments" 0
check_contains "B installed + no arguments" "already installed at $real_pin; nothing to do"
check_unchanged "B installed + no arguments" "$T1/external" "$fp"
[[ -f $T1/external/YRpp.sha256 ]] \
  && pass "B installed + no arguments: manifest still there" \
  || fail "B installed + no arguments: manifest was deleted"

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
[[ $C1 =~ ^[0-9a-f]{40}$ ]] || die "fixture checkout did not produce a full commit id"

# Same content without .git: a source whose revision can only be claimed; the
# content manifest is what has to catch content drift for it.
NOGIT="$work/yrpp-nogit"
mkdir -p "$NOGIT"
cp "$REPO/YRPP.h" "$REPO/StaticInits.cpp" "$NOGIT/"

# ---------------------------------------------------------------------------
# C) --from <git checkout at the pin> --write-manifest: installs, generates the
#    content manifest
# ---------------------------------------------------------------------------
T2="$work/fixture"
make_fixture_tree "$T2" "$C1"
fetch_out="$work/c.out"
run_fetch "$T2" "$fetch_out" --from "$REPO" --write-manifest
check_rc "C git source at pin + --write-manifest" 0
check_contains "C git source at pin + --write-manifest" "wrote external/YRpp.sha256"
check_installed "C git source at pin + --write-manifest" "$T2" "$C1"
cmp -s "$REPO/YRPP.h" "$T2/external/YRpp/YRPP.h" \
  && pass "C git source at pin: content copied" \
  || fail "C git source at pin: copied content differs"
grep -qF '  YRPP.h' "$T2/external/YRpp.sha256" \
  && pass "C: generated manifest lists YRPP.h" \
  || fail "C: generated manifest does not list YRPP.h"
cp "$T2/external/YRpp.sha256" "$work/manifest-c1"

# ---------------------------------------------------------------------------
# C2) existing manifest matches the source: a normal re-fetch installs it after
#     verifying the staged copy (no --write-manifest needed)
# ---------------------------------------------------------------------------
fetch_out="$work/c2.out"
run_fetch "$T2" "$fetch_out" --from "$REPO"
check_rc "C2 git source at pin, manifest matches" 0
check_contains "C2 git source at pin, manifest matches" "verifying the staged copy"
check_installed "C2 git source at pin, manifest matches" "$T2" "$C1"
check_same_manifest "C2 git source at pin, manifest matches" "$work/manifest-c1" "$T2/external/YRpp.sha256"

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

# ---------------------------------------------------------------------------
# F) source without .git + --commit: installs because the CONTENT is verified
#    against the manifest; the revision itself is only the claimed commit
# ---------------------------------------------------------------------------
rm -rf "$T2/external/YRpp" "$T2/external/YRpp.commit"
fetch_out="$work/f.out"
run_fetch "$T2" "$fetch_out" --from "$NOGIT" --commit "$C1"
check_rc "F no-.git source + --commit" 0
check_contains "F no-.git source + --commit" "not a git checkout"
check_contains "F no-.git source + --commit" "claimed commit"
check_contains "F no-.git source + --commit" "verified against external/YRpp.sha256"
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

# ---------------------------------------------------------------------------
# H) external/YRpp is a git checkout (no stamp) + no args: refuse, no deletion
#    (the D-09 variant: the implicit source search finds the installed tree)
# ---------------------------------------------------------------------------
T4="$work/git-installed"
make_fixture_tree "$T4" "$C1"
cp -a "$REPO" "$T4/external/YRpp"
cp "$work/manifest-c1" "$T4/external/YRpp.sha256"
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
# I) tampered installed content: the revision stamp still matches the pin, but
#    the content does not match the manifest -> both the gate and the fetch fail
# ---------------------------------------------------------------------------
T5="$work/tampered"
make_mirror_tree "$T5"
printf '\n// tampered header\n' >> "$T5/external/YRpp/YRPP.h"
run_check "$T5" "$work/i.check.out"
check_check_fails "I tampered installed content" "does not match"
if grep -qF 'changed: YRPP.h' "$work/i.check.out"; then
  pass "I tampered installed content: check_yrpp.py names the changed file"
else
  fail "I tampered installed content: check_yrpp.py does not name YRPP.h"
  sed 's/^/    | /' "$work/i.check.out" >&2
fi
run_check "$T5" "$work/i.tree.check.out" --verify-tree "$T5/external/YRpp"
check_check_fails "I --verify-tree on a tampered tree" "does not match"
fp="$(fingerprint "$T5/external")"
fetch_out="$work/i.out"
run_fetch "$T5" "$fetch_out"
check_refused "I fetch with tampered installed content"
check_contains "I fetch with tampered installed content" "rejects it"
check_unchanged "I fetch with tampered installed content" "$T5/external" "$fp"

# ---------------------------------------------------------------------------
# J) short commit stamp: a 12-hex prefix of the pin used to pass the old
#    bidirectional prefix match -> the full 40-hex id is required
# ---------------------------------------------------------------------------
T6="$work/short-stamp"
make_mirror_tree "$T6"
printf '%s\n' "${real_pin:0:12}" > "$T6/external/YRpp.commit"
run_check "$T6" "$work/j.check.out"
check_check_fails "J short stamp" "full 40-hex"
fp="$(fingerprint "$T6/external")"
fetch_out="$work/j.out"
run_fetch "$T6" "$fetch_out"
check_refused "J short stamp + fetch"
check_unchanged "J short stamp + fetch" "$T6/external" "$fp"

# ---------------------------------------------------------------------------
# K) missing manifest: the content pin cannot be verified -> refused; the
#    explicit --write-manifest regenerates it identically to the tracked one
# ---------------------------------------------------------------------------
T7="$work/missing-manifest"
make_mirror_tree "$T7"
rm -f "$T7/external/YRpp.sha256"
run_check "$T7" "$work/k.check.out"
check_check_fails "K missing manifest" "missing"
fp="$(fingerprint "$T7/external")"
fetch_out="$work/k.out"
run_fetch "$T7" "$fetch_out"
check_refused "K missing manifest + fetch"
check_contains "K missing manifest + fetch" "cannot be verified"
check_contains "K missing manifest + fetch" "--write-manifest"
check_unchanged "K missing manifest + fetch" "$T7/external" "$fp"
fetch_out="$work/k2.out"
run_fetch "$T7" "$fetch_out" --write-manifest
check_rc "K missing manifest + --write-manifest" 0
[[ -f $T7/external/YRpp.sha256 ]] \
  && pass "K missing manifest + --write-manifest: manifest regenerated" \
  || fail "K missing manifest + --write-manifest: manifest is still missing"
check_same_manifest "K missing manifest + --write-manifest" "$REAL_MANIFEST" "$T7/external/YRpp.sha256"
run_check "$T7" "$work/k3.check.out"
check_check_ok "K missing manifest + --write-manifest"

# ---------------------------------------------------------------------------
# L) short pin in EnginePin.h: only the full 40-hex commit id is accepted
# ---------------------------------------------------------------------------
T8="$work/short-pin"
make_fixture_tree "$T8" "${C1:0:8}"
run_check "$T8" "$work/l.check.out"
check_check_fails "L short pin" "full 40-hex"
fetch_out="$work/l.out"
run_fetch "$T8" "$fetch_out" --from "$REPO"
check_refused "L short pin + fetch"
check_contains "L short pin + fetch" "full 40-hex"

# ---------------------------------------------------------------------------
# P) inconsistent VC_YRPP_PIN_SHORT: the prefix is display-only, so it has to be
#    the first 8 hex digits of the pin (a drift would make the log and the
#    version resource lie about the pinned revision)
# ---------------------------------------------------------------------------
T10="$work/bad-short"
make_mirror_tree "$T10"
python3 - "$T10/src/Core/EnginePin.h" <<'PY'
import sys
path = sys.argv[1]
lines = []
for line in open(path, "r", encoding="utf-8"):
    if line.startswith("#define VC_YRPP_PIN_SHORT "):
        line = '#define VC_YRPP_PIN_SHORT "deadbeef"\n'
    lines.append(line)
open(path, "w", encoding="utf-8", newline="").write("".join(lines))
PY
run_check "$T10" "$work/p.check.out"
check_check_fails "P inconsistent VC_YRPP_PIN_SHORT" "display-only"
fp="$(fingerprint "$T10/external")"
fetch_out="$work/p.out"
run_fetch "$T10" "$fetch_out"
check_refused "P inconsistent VC_YRPP_PIN_SHORT + fetch"
check_unchanged "P inconsistent VC_YRPP_PIN_SHORT + fetch" "$T10/external" "$fp"

# ---------------------------------------------------------------------------
# M) dirty installed git checkout: HEAD is the pin, content is not the commit
# ---------------------------------------------------------------------------
T9="$work/dirty-installed"
make_fixture_tree "$T9" "$C1"
cp -a "$REPO" "$T9/external/YRpp"
git -C "$T9/external/YRpp" checkout -q "$C1"
run_check "$T9" "$work/m0.check.out" --write-manifest
check_check_ok "M clean git checkout + --write-manifest"
run_check "$T9" "$work/m1.check.out"
check_check_ok "M clean git checkout"
printf '\n// dirty\n' >> "$T9/external/YRpp/YRPP.h"
run_check "$T9" "$work/m2.check.out"
check_check_fails "M modified file in a git checkout" "dirty"
git -C "$T9/external/YRpp" checkout -q -- YRPP.h
printf '// untracked\n' > "$T9/external/YRpp/untracked.txt"
run_check "$T9" "$work/m3.check.out"
check_check_fails "M untracked file in a git checkout" "dirty"

# ---------------------------------------------------------------------------
# O) pin bump with a stale manifest: the staged copy is refused BEFORE the swap,
#    and --write-manifest then installs the new content and updates the manifest
# ---------------------------------------------------------------------------
write_fixture_pin "$T2" "$C2"
run_check "$T2" "$work/o0.check.out"
check_check_fails "O pin bumped, installed tree still at the old revision" "revision mismatch"
fp="$(fingerprint "$T2/external")"
fetch_out="$work/o1.out"
run_fetch "$T2" "$fetch_out" --from "$REPO"
check_refused "O pin bump vs stale manifest"
check_contains "O pin bump vs stale manifest" "does not match external/YRpp.sha256"
check_contains "O pin bump vs stale manifest" "installed external/YRpp is unchanged"
check_unchanged "O pin bump vs stale manifest" "$T2/external" "$fp"
[[ ! -e $T2/external/YRpp/extra.txt ]] \
  && pass "O pin bump vs stale manifest: the unpinned content was not installed" \
  || fail "O pin bump vs stale manifest: extra.txt was installed despite the refusal"
fetch_out="$work/o2.out"
run_fetch "$T2" "$fetch_out" --from "$REPO" --write-manifest
check_rc "O pin bump + --write-manifest" 0
check_installed "O pin bump + --write-manifest" "$T2" "$C2"
[[ -f $T2/external/YRpp/extra.txt ]] \
  && pass "O pin bump + --write-manifest: new content installed" \
  || fail "O pin bump + --write-manifest: new content is missing"
grep -qF '  extra.txt' "$T2/external/YRpp.sha256" \
  && pass "O pin bump + --write-manifest: manifest lists the new file" \
  || fail "O pin bump + --write-manifest: manifest does not list extra.txt"

# ---------------------------------------------------------------------------
# N) dirty source git checkout: the content is not the claimed commit -> refused
# ---------------------------------------------------------------------------
printf '// untracked\n' > "$REPO/untracked.tmp"
fp="$(fingerprint "$T2/external")"
fetch_out="$work/n.out"
run_fetch "$T2" "$fetch_out" --from "$REPO"
check_refused "N dirty source checkout"
check_contains "N dirty source checkout" "is dirty"
check_unchanged "N dirty source checkout" "$T2/external" "$fp"
rm -f "$REPO/untracked.tmp"

# ---------------------------------------------------------------------------
# The real tree must be untouched: no staging directories, same stamp, manifest
# and files
# ---------------------------------------------------------------------------
printf '\n== integrity of the real installed tree\n'
real_pin_now="$(realpin "$VCS_ROOT/src/Core/EnginePin.h")"
[[ $real_pin_now == "$real_pin" ]] \
  && pass "real tree: pin unchanged" \
  || fail "real tree: pin changed"
[[ "$(tr -d '[:space:]' < "$REAL_STAMP")" == "$real_stamp_start" ]] \
  && pass "real tree: external/YRpp.commit unchanged" \
  || fail "real tree: external/YRpp.commit changed"
[[ "$(md5sum "$REAL_MANIFEST" | cut -d' ' -f1)" == "$real_manifest_hash_start" ]] \
  && pass "real tree: external/YRpp.sha256 unchanged" \
  || fail "real tree: external/YRpp.sha256 changed"
fp="$(fingerprint "$REAL_DEST")"
[[ $fp == "$real_fp_start" ]] \
  && pass "real tree: external/YRpp unchanged" \
  || fail "real tree: external/YRpp changed"
[[ "$(count_files "$REAL_DEST")" == "$real_files_start" ]] \
  && pass "real tree: external/YRpp still $real_files_start files" \
  || fail "real tree: external/YRpp file count changed"
run_check "$VCS_ROOT" "$work/real.check.out"
check_check_ok "real tree"
leftovers="$(find "$VCS_ROOT/external" -maxdepth 1 \( -name '.YRpp.new.*' -o -name '.YRpp.old.*' -o -name 'YRpp.commit.new.*' -o -name '.YRpp.sha256.new.*' \) -print 2>/dev/null)"
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
