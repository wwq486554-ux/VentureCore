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
# gitlink); this script keeps working either way because check_yrpp.py accepts
# both a git checkout and the external/YRpp.commit stamp written here.
#
# Usage:
#   scripts/linux/fetch_yrpp.sh --from PATH     # copy this YRpp checkout
#   scripts/linux/fetch_yrpp.sh --from PATH --commit SHA
#                                               # same, for a checkout without .git
#
# Environment:
#   VCS_YRPP_SRC    YRpp checkout to copy (same as --from)
#
# The copy source is deliberately not hard-coded: YRpp is not distributed with
# this repository, so every machine has to point at its own checkout (or fetch it
# with --commit).

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCS_ROOT="$(cd "$VCS_LINUX_DIR/../.." && pwd)"
DEST="$VCS_ROOT/external/YRpp"
STAMP="$VCS_ROOT/external/YRpp.commit"

vcs_info() { printf '[fetch-yrpp] %s\n' "$*" >&2; }
vcs_die() { printf '[fetch-yrpp] error: %s\n' "$*" >&2; exit 1; }

from=""
commit=""
while (( $# )); do
  case "$1" in
    --from)   from="${2:?--from needs a path}"; shift ;;
    --commit) commit="${2:?--commit needs a revision}"; shift ;;
    -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) vcs_die "unknown argument: $1" ;;
  esac
  shift
done

expected="$(sed -n 's/^#define[[:space:]]\{1,\}VC_YRPP_PIN[[:space:]]\{1,\}"\([0-9a-fA-F]*\)".*/\1/p' "$VCS_ROOT/src/Core/EnginePin.h")"
[[ -n $expected ]] || vcs_die "VC_YRPP_PIN not found in src/Core/EnginePin.h"

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

src_commit=""
if [[ -e "$from/.git" ]]; then
  src_commit="$(git -C "$from" rev-parse HEAD 2>/dev/null || true)"
fi
if [[ -n $commit ]]; then
  src_commit="$commit"
fi
[[ -n $src_commit ]] || vcs_die \
  "cannot determine the revision of '$from' (not a git checkout); pass --commit SHA"
if [[ $src_commit != "$expected" ]]; then
  vcs_die "source checkout is at $src_commit, but the pin is $expected.
  Refusing to copy an unpinned YRpp. Update the pin in src/Core/EnginePin.h
  (with a compatibility review) or point --from at the pinned commit."
fi

vcs_info "copying $from ($src_commit) -> external/YRpp"
rm -rf "$DEST"
mkdir -p "$VCS_ROOT/external"
cp -a "$from/." "$DEST/"
rm -rf "$DEST/.git"
printf '%s\n' "$src_commit" > "$STAMP"
python3 "$VCS_LINUX_DIR/check_yrpp.py"
vcs_info "done"
