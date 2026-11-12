#!/usr/bin/env bash
# Fetch (or refresh) the pinned YRpp headers into external/YRpp.
#
# YRpp has no license declaration (the community treats it as all-rights-reserved
# but uses it in practice), so it is NOT vendored in this repository.  It is an
# external build dependency, pinned by commit in src/Core/EnginePin.h.
#
# This helper is the "no network needed" path used during local development: it
# copies a YRpp checkout that already exists on the machine.  Once the repository
# is public the preferred path is a git submodule (the pin then lives in the
# gitlink); this script keeps working either way.
#
# Two pins are enforced, not one:
#   * the revision - the source must be the full 40-hex commit in
#     src/Core/EnginePin.h (VC_YRPP_PIN), and a source git checkout must be clean;
#   * the content - every file of the installed tree must match
#     external/YRpp.sha256, the sha256 manifest tracked in this repository (see
#     scripts/linux/check_yrpp.py).  The staged copy is verified against the
#     manifest BEFORE it replaces the installed tree, so a source tree without a
#     .git (whose content cannot be tied to a commit) can no longer install
#     arbitrary bytes under a claimed commit.
#
# Usage:
#   scripts/linux/fetch_yrpp.sh                 # install from a checkout found on
#                                               # this machine; a no-op when
#                                               # external/YRpp is already at the pin
#   scripts/linux/fetch_yrpp.sh --from PATH     # copy this YRpp checkout
#   scripts/linux/fetch_yrpp.sh --from PATH --commit SHA
#                                               # only for a checkout without .git
#   scripts/linux/fetch_yrpp.sh --write-manifest
#                                               # after a deliberate pin/content
#                                               # review: accept the source content,
#                                               # install it and regenerate
#                                               # external/YRpp.sha256
#
# Environment:
#   VCS_YRPP_SRC    YRpp checkout to copy (same as --from)
#
# Safety rules:
#   * external/YRpp is never used as its own source: copying the installed tree
#     onto itself would delete it first.  That is now a no-op when it is already
#     installed at the pin, and an error otherwise.
#   * The copy is staged in external/.YRpp.new.<pid> and moved into place only
#     after a complete copy and a content-manifest check, so a failed fetch
#     leaves the installed tree intact.
#   * --commit never overrides the HEAD of a source that is a git checkout; if
#     both are given and differ, the fetch is refused.  A dirty source checkout
#     is refused as well: its content is not the commit.  Without a .git entry
#     the revision can only be the claimed commit, so the content manifest is
#     what pins such a source.
#   * Without --write-manifest the tracked external/YRpp.sha256 must exist and
#     the source content must match it; only --write-manifest accepts new content
#     (and then refreshes the manifest).
#
# The copy source is deliberately not hard-coded: YRpp is not distributed with
# this repository, so every machine has to point at its own checkout (or fetch it
# with --commit).

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCS_ROOT="$(cd "$VCS_LINUX_DIR/../.." && pwd)"
DEST="$VCS_ROOT/external/YRpp"
STAMP="$VCS_ROOT/external/YRpp.commit"
MANIFEST="$VCS_ROOT/external/YRpp.sha256"

vcs_info() { printf '[fetch-yrpp] %s\n' "$*" >&2; }
vcs_warn() { printf '[fetch-yrpp] warning: %s\n' "$*" >&2; }
vcs_die()  { printf '[fetch-yrpp] error: %s\n' "$*" >&2; exit 1; }
usage() { awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "${BASH_SOURCE[0]}"; }

# Lowercase a hex string; commits are compared case-insensitively.
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

from=""
commit=""
write_manifest=0
while (( $# )); do
  case "$1" in
    --from)   from="${2:?--from needs a path}"; shift ;;
    --commit) commit="${2:?--commit needs a revision}"; shift ;;
    --write-manifest) write_manifest=1 ;;
    -h|--help) usage; exit 0 ;;
    *) vcs_die "unknown argument: $1" ;;
  esac
  shift
done

expected="$(sed -n 's/^#define[[:space:]]\{1,\}VC_YRPP_PIN[[:space:]]\{1,\}"\([0-9a-fA-F]*\)".*/\1/p' "$VCS_ROOT/src/Core/EnginePin.h")"
[[ -n $expected ]] || vcs_die "VC_YRPP_PIN not found in src/Core/EnginePin.h"
expected="$(lower "$expected")"
commit="$(lower "$commit")"

# The pin is the revision: a 7- or 12-hex prefix matches several real commits and
# a short stamp hides a changed tree, so only the full 40-hex id is accepted.
# (VC_YRPP_PIN_SHORT exists for log/version display only.)
if ! [[ $expected =~ ^[0-9a-f]{40}$ ]]; then
  vcs_die "VC_YRPP_PIN must be the full 40-hex commit id (got '$expected', ${#expected} hex digit(s)).
  A short id matches several revisions. Use VC_YRPP_PIN_SHORT for display only."
fi

# The content pin has to exist to be checked; only --write-manifest may create
# or replace it (that is the explicit "accept this content" step).
if (( write_manifest == 0 )) && [[ ! -f $MANIFEST ]]; then
  vcs_die "external/YRpp.sha256 is missing: the pinned content cannot be verified.
  If this pin/content was just reviewed and accepted, run:
      scripts/linux/fetch_yrpp.sh --write-manifest
  (and commit the updated manifest)."
fi

# An explicitly named source (--from or VCS_YRPP_SRC) is a usage decision, not
# the "refresh whatever is installed" default; the self-copy guard distinguishes
# the two so that an explicit self-copy is always an error.
explicit_from=0
[[ -n $from || -n ${VCS_YRPP_SRC:-} ]] && explicit_from=1

if [[ -z $from ]]; then
  for candidate in \
    "${VCS_YRPP_SRC:-}" \
    "$VCS_ROOT/../YRpp" \
    "$VCS_ROOT/external/YRpp" ; do
    [[ -n $candidate && -f "$candidate/YRPP.h" ]] || continue
    from="$candidate"
    break
  done
fi
[[ -n $from && -f "$from/YRPP.h" ]] || vcs_die \
  "no local YRpp checkout found; pass --from PATH or set VCS_YRPP_SRC"

# ---------------------------------------------------------------------------
# Self-copy guard.
#
# The implicit source search above ends with external/YRpp itself, so "install
# the already installed tree" used to be indistinguishable from a normal fetch -
# and the copy deleted the destination before reading it.  Compare normalised
# paths *before* touching anything: this is either a no-op or a hard error.
# ---------------------------------------------------------------------------
from_real="$(realpath -m -- "$from")"
dest_real="$(realpath -m -- "$DEST")"
if [[ $from_real == "$dest_real" ]]; then
  installed_rev=""
  if [[ -f $STAMP ]]; then
    installed_rev="$(lower "$(tr -d '[:space:]' < "$STAMP")")"
  fi
  if (( explicit_from == 0 )) && [[ -n $installed_rev && $installed_rev == "$expected" ]]; then
    # No source was named and the only checkout found is the installed tree:
    # that is the "refresh what is already there" invocation, which is a no-op.
    manifest_args=()
    (( write_manifest )) && manifest_args=(--write-manifest)
    python3 "$VCS_LINUX_DIR/check_yrpp.py" "${manifest_args[@]}" \
      || vcs_die "external/YRpp claims the pin but check_yrpp.py rejects it"
    vcs_info "already installed at $expected; nothing to do"
    exit 0
  fi
  if [[ -n $installed_rev && $installed_rev == "$expected" ]]; then
    vcs_die "refusing to copy external/YRpp onto itself: it is already installed at $expected.
  There is nothing to do; run the script without --from to check the installation,
  or point --from at a YRpp checkout on this machine."
  fi
  vcs_die "refusing to copy '$from' onto itself: source and destination are both external/YRpp.
  The installed tree is not stamped with the pinned revision ($expected), so it
  cannot be refreshed from itself.  Point --from at a YRpp checkout on this
  machine, or delete external/YRpp (and external/YRpp.commit) and fetch again."
fi

# ---------------------------------------------------------------------------
# Revision of the copy source.
#
# A git checkout is the only source whose *content* can be tied to a commit, so
# its HEAD is authoritative: --commit must agree with it instead of replacing it
# (a wrong --commit used to make the pin gate accept an arbitrary tree), and a
# dirty checkout is refused because its content is not the commit.  A source
# tree without .git can only claim a revision; the content manifest is the gate
# for it.
# ---------------------------------------------------------------------------
src_commit=""
if [[ -e "$from/.git" ]]; then
  src_commit="$(git -C "$from" rev-parse HEAD 2>/dev/null || true)"
  if [[ -z $src_commit ]]; then
    vcs_die "source '$from' looks like a git checkout but its HEAD cannot be determined (broken or incomplete .git); refusing to guess its revision"
  fi
  src_commit="$(lower "$src_commit")"
  src_dirty="$(git -C "$from" status --porcelain --untracked-files=normal 2>/dev/null || true)"
  if [[ -n $src_dirty ]]; then
    vcs_die "source checkout '$from' is dirty: its working tree is not commit $src_commit.
  A dirty checkout would install content that no commit describes.  Clean it
  (commit, stash or discard the changes and untracked files) and retry.
  First entries:
$(printf '%s\n' "$src_dirty" | sed -n '1,5p' | sed 's/^/    /')"
  fi
  if [[ -n $commit && $commit != "$src_commit" ]]; then
    vcs_die "source checkout is a git repository at HEAD $src_commit, but --commit says $commit.
  --commit only names the revision of a source *without* .git; it never overrides
  a checkout's HEAD.  Check out the pinned commit in '$from', or pass a source
  that is not a git checkout."
  fi
elif [[ -n $commit ]]; then
  src_commit="$commit"
  if ! [[ $src_commit =~ ^[0-9a-f]{40}$ ]]; then
    vcs_die "--commit must be the full 40-hex commit id (got '$src_commit')"
  fi
  if [[ -f $MANIFEST ]] && (( write_manifest == 0 )); then
    vcs_warn "source '$from' is not a git checkout: its revision is only the claimed commit $src_commit; its content is verified against external/YRpp.sha256"
  else
    vcs_warn "source '$from' is not a git checkout: its revision is only the claimed commit $src_commit, and --write-manifest accepts its content without a manifest check"
  fi
else
  vcs_die "cannot determine the revision of '$from' (not a git checkout); pass --commit SHA"
fi

[[ $src_commit == "$expected" ]] || vcs_die "source checkout is at $src_commit, but the pin is $expected.
  Refusing to copy an unpinned YRpp. Update the pin in src/Core/EnginePin.h
  (with a compatibility review) or point --from at the pinned commit."

# ---------------------------------------------------------------------------
# Staged copy + content check + swap.
#
# cp -a writes into external/.YRpp.new.<pid>; the installed directory is only
# replaced (by a same-directory rename) after the copy completed *and* the staged
# content matched external/YRpp.sha256, and is moved back when the swap fails.
# An interrupted or refused fetch therefore leaves either the old tree or a
# complete new tree - never a half-copied external/YRpp and never content the
# tracked manifest does not describe.
# ---------------------------------------------------------------------------
external_dir="$VCS_ROOT/external"
mkdir -p "$external_dir"

tmp="$external_dir/.YRpp.new.$$"
old="$external_dir/.YRpp.old.$$"
stamp_tmp="$STAMP.new.$$"
cleanup() {
  [[ -n ${tmp:-} && -e ${tmp:-} ]] && rm -rf "$tmp"
  [[ -n ${old:-} && -e ${old:-} ]] && rm -rf "$old"
  [[ -n ${stamp_tmp:-} && -e ${stamp_tmp:-} ]] && rm -f "$stamp_tmp"
  return 0
}
trap cleanup EXIT

rm -rf "$tmp" "$old"
mkdir -p "$tmp"
vcs_info "copying $from ($src_commit) -> external/YRpp"
if ! cp -a "$from/." "$tmp/"; then
  vcs_die "copying '$from' failed; the installed external/YRpp is unchanged"
fi
rm -rf "$tmp/.git"

if [[ -f $MANIFEST ]] && (( write_manifest == 0 )); then
  vcs_info "verifying the staged copy against external/YRpp.sha256"
  if ! python3 "$VCS_LINUX_DIR/check_yrpp.py" --verify-tree "$tmp"; then
    vcs_die "the staged copy of '$from' does not match external/YRpp.sha256.
  Refusing to install it; the installed external/YRpp is unchanged.
  Restore the source checkout to the pinned revision (its content must be the
  reviewed content), or - only after a deliberate content review and a pin
  update - re-run with --write-manifest to accept the new content."
  fi
fi

if [[ -e $DEST ]]; then
  mv "$DEST" "$old" || vcs_die "cannot move the installed external/YRpp aside; it is unchanged"
  if ! mv "$tmp" "$DEST"; then
    mv "$old" "$DEST" 2>/dev/null || true
    vcs_die "cannot move the staged copy into place; the installed tree was restored"
  fi
  tmp=""
  rm -rf "$old"
  old=""
else
  mv "$tmp" "$DEST" || vcs_die "cannot move the staged copy into place; no installed tree was touched"
  tmp=""
fi

printf '%s\n' "$src_commit" > "$stamp_tmp"
mv -f "$stamp_tmp" "$STAMP"
stamp_tmp=""

manifest_args=()
(( write_manifest )) && manifest_args=(--write-manifest)
python3 "$VCS_LINUX_DIR/check_yrpp.py" "${manifest_args[@]}" \
  || vcs_die "the installed tree failed the pin/content gate"
vcs_info "done"
