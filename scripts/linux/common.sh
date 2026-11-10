#!/usr/bin/env bash
# Shared configuration and helpers for the Linux-side VentureCore build.
#
# This build drives the *real* MSVC toolchain (cl.exe / rc.exe / link.exe) that
# is already installed on the Windows system, running it under Wine. Nothing is
# downloaded and no separate compiler is introduced, so the produced
# VentureCore.dll is built by the exact same toolset as the Windows/CI build.
#
# Source this file; do not execute it.

# shellcheck shell=bash

set -o pipefail

vcs_info() { printf '[linux-build] %s\n' "$*" >&2; }
vcs_warn() { printf '[linux-build] warning: %s\n' "$*" >&2; }
vcs_die()  { printf '[linux-build] error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Layout / user-tunable settings (all overridable from the environment)
# ---------------------------------------------------------------------------

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCS_ROOT="$(cd "$VCS_LINUX_DIR/../.." && pwd)"

# True when $1 is a directory this user can actually create files in. A plain
# [[ -w ]] test is not enough: the mount itself may be read-only.
vcs_dir_writable() {
  local probe
  [[ -d $1 ]] || return 1
  probe="$1/.pbs-write-probe.$$"
  if { : >"$probe"; } 2>/dev/null; then
    rm -f "$probe"
    return 0
  fi
  return 1
}

# Wine prefix. MUST live on a writable Linux-native filesystem
# (ext4/btrfs/xfs/tmpfs/...). Two different setups both surface as a compiler
# error that looks unrelated to the environment:
#   * NTFS/FUSE (ntfs-3g, exfat, ...) -> "error D8037: cannot create temporary
#     il file", because of how those filesystems handle the compiler's temp
#     files;
#   * a read-only mount -> the same D8037, or "error D8050: cannot execute
#     c1xx.dll", because cl.exe cannot create its temp files at all.
# Prefer $HOME/.cache, but fall back to a directory next to the source tree when
# $HOME is not writable (read-only root, sandbox, container), so that the
# default keeps working without any environment setup.
if [[ -z ${VCS_WINEPREFIX:-} ]]; then
  if vcs_dir_writable "$HOME/.cache"; then
    VCS_WINEPREFIX="$HOME/.cache/venturecore-wine"
  else
    VCS_WINEPREFIX="$(cd "$VCS_ROOT/.." && pwd)/.wine-venturecore"
  fi
fi

# MSVC toolset to use. 14.44.35207 is the last v143 (VS2022) toolset and is what
# VentureCore.props' <PlatformToolset>v143</PlatformToolset> maps to on the current
# machine; override if the Windows install differs.
: "${VCS_MSVC_VER:=14.44.35207}"
: "${VCS_MSVC_ARCH:=x86}"          # target architecture
: "${VCS_SDK_VER:=}"               # empty = newest Windows 10 SDK found
: "${VCS_WINDOWS_ROOT:=}"          # empty = autodetect
: "${VCS_JOBS:=$(nproc 2>/dev/null || echo 4)}"
: "${VCS_DEBUG_INFO:=z7}"          # z7 (robust under Wine) | zi (MSBuild default)
: "${VCS_MODULES:=0}"              # 1 = also pass /experimental:module (see scripts/linux/README.md)

# ---------------------------------------------------------------------------
# Locating the Windows installation
# ---------------------------------------------------------------------------

# Prints the root of the mounted Windows system volume (the one that holds
# "Program Files"), or fails.
vcs_find_windows_root() {
  local c
  if [[ -n $VCS_WINDOWS_ROOT ]]; then
    [[ -d "$VCS_WINDOWS_ROOT" ]] || vcs_die "VCS_WINDOWS_ROOT=$VCS_WINDOWS_ROOT does not exist"
    printf '%s' "$VCS_WINDOWS_ROOT"; return 0
  fi
  for c in /mnt/Windows-SSD /mnt/windows /mnt/c /mnt/C /mnt/os \
           /run/media/*/* /media/* ; do
    if [[ -d "$c/Program Files/Microsoft Visual Studio" || -d "$c/Program Files (x86)/Windows Kits" ]]; then
      printf '%s' "$c"; return 0
    fi
  done
  return 1
}

# Sets: VCS_WINDOWS_ROOT VCS_VS_ROOT VCS_MSVC_DIR VCS_VC_INCLUDE VCS_VC_LIB
#       VCS_VC_BIN VCS_VC_ATL_INCLUDE VCS_VC_ATL_LIB
vcs_locate_msvc() {
  local win vs_root kind ed
  win="$(vcs_find_windows_root)" || vcs_die \
    "could not find the Windows volume. Mount it (e.g. /mnt/Windows-SSD) or set VCS_WINDOWS_ROOT=/path"
  VCS_WINDOWS_ROOT="$win"

  for kind in "Program Files" "Program Files (x86)"; do
    for ed in Community Professional Enterprise BuildTools Preview; do
      for vs_root in "$win/$kind/Microsoft Visual Studio"/*/"$ed"; do
        [[ -d "$vs_root/VC/Tools/MSVC" ]] && { VCS_VS_ROOT="$vs_root"; break 3; }
      done
    done
  done
  [[ -n ${VCS_VS_ROOT:-} ]] || vcs_die "no Visual Studio C++ tools found under '$win/Program Files/Microsoft Visual Studio'"

  VCS_MSVC_DIR="$VCS_VS_ROOT/VC/Tools/MSVC/$VCS_MSVC_VER"
  if [[ ! -d $VCS_MSVC_DIR ]]; then
    vcs_warn "MSVC $VCS_MSVC_VER not installed; available:"
    ls -1 "$VCS_VS_ROOT/VC/Tools/MSVC" >&2 || true
    vcs_die "set VCS_MSVC_VER to one of the versions above"
  fi

  VCS_VC_INCLUDE="$VCS_MSVC_DIR/include"
  VCS_VC_LIB="$VCS_MSVC_DIR/lib/$VCS_MSVC_ARCH"
  VCS_VC_ATL_INCLUDE="$VCS_MSVC_DIR/atlmfc/include"
  VCS_VC_ATL_LIB="$VCS_MSVC_DIR/atlmfc/lib/$VCS_MSVC_ARCH"

  # Files on the Windows volume are mounted without the execute bit, so test
  # for readability only - Wine does not need +x.
  if [[ -f "$VCS_MSVC_DIR/bin/HostX64/$VCS_MSVC_ARCH/cl.exe" ]]; then
    VCS_VC_BIN="$VCS_MSVC_DIR/bin/HostX64/$VCS_MSVC_ARCH"
  elif [[ -f "$VCS_MSVC_DIR/bin/HostX86/$VCS_MSVC_ARCH/cl.exe" ]]; then
    VCS_VC_BIN="$VCS_MSVC_DIR/bin/HostX86/$VCS_MSVC_ARCH"
  else
    vcs_die "cl.exe for $VCS_MSVC_ARCH not found under $VCS_MSVC_DIR/bin"
  fi
}

# Sets: VCS_SDK_ROOT VCS_SDK_INC VCS_SDK_LIB VCS_SDK_RC
vcs_locate_sdk() {
  local kind root v versions
  for kind in "Program Files (x86)" "Program Files"; do
    for root in "$VCS_WINDOWS_ROOT/$kind/Windows Kits/10" "$VCS_WINDOWS_ROOT/Windows Kits/10"; do
      [[ -d "$root/Include" ]] && { VCS_SDK_ROOT="$root"; break 2; }
    done
  done
  [[ -n ${VCS_SDK_ROOT:-} ]] || vcs_die "Windows 10/11 SDK not found under '$VCS_WINDOWS_ROOT'"

  if [[ -n $VCS_SDK_VER ]]; then
    versions="$VCS_SDK_VER"
  else
    versions="$(ls -1 "$VCS_SDK_ROOT/Include" 2>/dev/null | sort -Vr)"
  fi
  for v in $versions; do
    [[ -d "$VCS_SDK_ROOT/Include/$v/um" && -d "$VCS_SDK_ROOT/Lib/$v/um/$VCS_MSVC_ARCH" ]] || continue
    VCS_SDK_VER="$v"; break
  done
  [[ -n ${VCS_SDK_VER:-} ]] || vcs_die "no usable Windows SDK version found under $VCS_SDK_ROOT/Include"

  VCS_SDK_INC="$VCS_SDK_ROOT/Include/$VCS_SDK_VER"
  VCS_SDK_LIB="$VCS_SDK_ROOT/Lib/$VCS_SDK_VER"
  VCS_SDK_RC="$VCS_SDK_ROOT/bin/$VCS_SDK_VER/$VCS_MSVC_ARCH/rc.exe"
  [[ -f $VCS_SDK_RC ]] || vcs_die "rc.exe not found at $VCS_SDK_RC"
}

# Everything at once, plus the include/lib directory arrays.
vcs_locate_toolchain() {
  vcs_locate_msvc
  vcs_locate_sdk

  VCS_INC_DIRS=(
    "$VCS_ROOT/src"
    "$VCS_ROOT/include"
    "$VCS_ROOT/external/YRpp"
    "$VCS_VC_INCLUDE"
    "$VCS_VC_ATL_INCLUDE"
    "$VCS_SDK_INC/ucrt"
    "$VCS_SDK_INC/shared"
    "$VCS_SDK_INC/um"
    "$VCS_SDK_INC/winrt"
  )
  VCS_LIB_DIRS=(
    "$VCS_SDK_LIB/um/$VCS_MSVC_ARCH"
    "$VCS_SDK_LIB/ucrt/$VCS_MSVC_ARCH"
    "$VCS_VC_LIB"
    "$VCS_VC_ATL_LIB"
  )

  local d
  for d in "${VCS_INC_DIRS[@]}"; do
    [[ -d $d ]] || vcs_die "include directory missing: $d"
  done
  for d in "${VCS_LIB_DIRS[@]}"; do
    [[ -d $d ]] || vcs_die "library directory missing: $d"
  done
}

# ---------------------------------------------------------------------------
# Wine plumbing
# ---------------------------------------------------------------------------

# Unix path -> Wine Z: path (the whole Linux root is visible as Z:\).
vcs_w() { printf 'Z:%s' "${1//\//\\}"; }

# ';'-joined Windows list for INCLUDE / LIB / PATH style variables.
vcs_w_list() {
  local out="" d
  for d in "$@"; do
    [[ -n $out ]] && out+=";"
    out+="$(vcs_w "$d")"
  done
  printf '%s' "$out"
}

vcs_check_prefix_fs() {
  local fstype
  fstype="$(stat -f -c %T "$(dirname "$VCS_WINEPREFIX")" 2>/dev/null || true)"
  case "$fstype" in
    fuseblk|ntfs|ntfs3|vfat|msdos|exfat|nfs|cifs|smb*|9p|fuse*)
      vcs_die "VCS_WINEPREFIX is on '$fstype', which breaks MSVC under Wine (error D8037).
  Put the Wine prefix on a Linux-native filesystem, e.g.
      VCS_WINEPREFIX=\$HOME/.cache/venturecore-wine
  Build outputs and the source tree may stay on NTFS; only the prefix (and its
  temp directory) must be native." ;;
  esac

  # The filesystem *type* can be right while the mount is still read-only (a
  # read-only bind mount, a sandboxed root, ...). That fails much later, deep
  # inside cl.exe, as a confusing D8037/D8050 - so check it up front. When the
  # prefix does not exist yet (setup) the directory that will hold it is checked.
  local target="$VCS_WINEPREFIX"
  [[ -d $target ]] || target="$(dirname "$VCS_WINEPREFIX")"
  vcs_dir_writable "$target" || vcs_die \
    "Wine prefix '$VCS_WINEPREFIX' is not writable ($target).
  cl.exe keeps its temp directory inside the prefix, so a read-only location
  makes it fail with 'error D8037' or 'error D8050'.
  Point VCS_WINEPREFIX at a writable directory on a native filesystem, e.g.
      VCS_WINEPREFIX=\$HOME/.cache/venturecore-wine"
}

# Wine derives the Windows codepages from LC_ALL only (LANG alone is ignored).
# With a non-Chinese LC_ALL every non-ASCII character of a localized MSVC
# message is replaced by "?" before it reaches the log, which makes the Chinese
# messages of a zh-CN Visual Studio install unreadable. Point LC_ALL at a
# Chinese locale when the host has one; VCS_WINE_LOCALE overrides the choice.
vcs_resolve_wine_locale() {
  [[ ${VCS_WINE_LOCALE_SET:-0} == 1 ]] && return 0
  if [[ -z ${VCS_WINE_LOCALE:-} ]] && locale -a 2>/dev/null | grep -qi '^zh_CN\.'; then
    VCS_WINE_LOCALE=zh_CN.UTF-8
  fi
  export VCS_WINE_LOCALE="${VCS_WINE_LOCALE:-}"
  export VCS_WINE_LOCALE_SET=1
}

# Exports WINEPREFIX/WINEDEBUG/TMP/TEMP and the INCLUDE/LIB lists for the tools.
# $1 = output dir whose Generated/ header must be on the include path (optional)
vcs_wine_env() {
  local gen_dir="${1:-}"
  vcs_check_prefix_fs
  export WINEPREFIX="$VCS_WINEPREFIX"
  export WINEDEBUG="${WINEDEBUG:--all}"
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"

  # Keep wineserver's socket inside a persistent, writable directory. Wine
  # otherwise uses /tmp/.wine-<uid>; when /tmp is cleared while a wineserver is
  # still alive (sandboxes and containers often do this between sessions) the
  # next wine process cannot reach it, starts a *second* wineserver on the same
  # prefix, and the registry can end up flushed truncated - which then shows up
  # as a bogus "error D8037" on every later compile. An inherited
  # XDG_RUNTIME_DIR is only used when it is actually writable (a read-only
  # /run/user/<uid> is common in these setups).
  local vcs_rt="${XDG_RUNTIME_DIR:-}"
  if [[ -z $vcs_rt ]] || ! vcs_dir_writable "$vcs_rt"; then
    vcs_rt="$VCS_WINEPREFIX/.wine-runtime"
  fi
  if mkdir -p "$vcs_rt" 2>/dev/null; then
    chmod 700 "$vcs_rt" 2>/dev/null || true
    export XDG_RUNTIME_DIR="$vcs_rt"
  fi
  vcs_resolve_wine_locale
  [[ -n ${VCS_WINE_LOCALE:-} ]] && export LC_ALL="$VCS_WINE_LOCALE"
  # MSVC needs its temp directory inside the (native-FS) prefix. Create it only
  # when the prefix really exists: creating drive_c here would make a bare,
  # never-initialised prefix look "ready" to vcs_wine_ready.
  export TMP='C:\venturecore-tmp'
  export TEMP='C:\venturecore-tmp'
  if [[ -d "$VCS_WINEPREFIX/drive_c" ]]; then
    mkdir -p "$VCS_WINEPREFIX/drive_c/venturecore-tmp"
  fi

  local inc=("${VCS_INC_DIRS[@]}")
  [[ -n $gen_dir ]] && inc=("$gen_dir" "${inc[@]}")
  export INCLUDE="$(vcs_w_list "${inc[@]}")"
  export LIB="$(vcs_w_list "${VCS_LIB_DIRS[@]}")"
}

# A prefix counts as initialised when wineboot has actually run in it. Checking
# for drive_c alone is not enough - an aborted run leaves an empty drive_c, and
# system.reg/user.reg are unreliable because Wine only flushes the registry
# files when the wineserver exits, so right after wineboot they may not exist.
vcs_wine_ready() {
  [[ -d "$VCS_WINEPREFIX/drive_c/windows" ]] || return 1
  [[ -e "$VCS_WINEPREFIX/system.reg" || -e "$VCS_WINEPREFIX/user.reg" \
     || -e "$VCS_WINEPREFIX/.update-timestamp" ]]
}

# ---------------------------------------------------------------------------
# Project file list (single source of truth: the Visual Studio project files)
# ---------------------------------------------------------------------------

# Prints one source path per line (relative to the repo root), tab separated
# from its object file path, for every ClCompile entry.
vcs_list_sources() {
  local vcxproj="$VCS_ROOT/VentureCore.vcxproj"
  local yrppprops="$VCS_ROOT/external/YRpp/YRpp.props"
  [[ -f $vcxproj ]] || vcs_die "missing $vcxproj"

  {
    grep -o 'ClCompile Include="[^"]*"' "$vcxproj" | sed 's/^ClCompile Include="//; s/"$//'
    if [[ -f $yrppprops ]]; then
      grep -o 'ClCompile Include="[^"]*"' "$yrppprops" | sed 's/^ClCompile Include="//; s/"$//' \
        | sed 's#\$(YRppDir)#external/YRpp#g'
    fi
  } | sed 's/\\/\//g' | sed 's#^\./##' | awk 'NF' | sort -u
}

# Same for ResourceCompile entries.
vcs_list_resources() {
  grep -o 'ResourceCompile Include="[^"]*"' "$VCS_ROOT/VentureCore.vcxproj" \
    | sed 's/^ResourceCompile Include="//; s/"$//' | sed 's/\\/\//g'
}

# ---------------------------------------------------------------------------
# Generated git stamp header
# ---------------------------------------------------------------------------
#
# src/Core/PlatformVersion.h includes "VentureCore.Git.h", which the build
# generates from the current git state into <IntDir>/Generated (both MSBuild's
# ComputeGitInfo target and the Linux script).  Anything that compiles platform
# sources outside the build - the unit tests and the benchmark - has to run the
# same generator first, otherwise the include cannot be resolved.
#
# $1 = output directory (the Generated dir); prints nothing when unchanged.
vcs_generate_git_header() {
  local gen_dir="$1" commit ref dirty tmp
  mkdir -p "$gen_dir"
  commit="$(git -C "$VCS_ROOT" rev-parse --short HEAD 2>/dev/null || true)"
  ref="$(git -C "$VCS_ROOT" symbolic-ref HEAD 2>/dev/null || true)"
  if [[ -z $ref && -n $commit ]]; then
    ref="$(git -C "$VCS_ROOT" for-each-ref --points-at HEAD --format='%(refname)' 2>/dev/null | sed -n '1p')"
  fi
  dirty=""
  if [[ -n $commit ]] && [[ -n $(git -C "$VCS_ROOT" status --porcelain 2>/dev/null) ]]; then
    dirty="-dirty"
  fi
  tmp="$gen_dir/VentureCore.Git.h.tmp"
  {
    printf '// Generated by ComputeGitInfo - do not edit.\n'
    if [[ -n $commit ]]; then printf '#define STR_GIT_COMMIT "%s"\n' "$commit"; fi
    if [[ -n $ref ]];    then printf '#define STR_GIT_REF "%s"\n' "$ref"; fi
    if [[ -n $dirty ]];  then printf '#define STR_GIT_DIRTY "%s"\n' "$dirty"; fi
  } > "$tmp"
  if [[ -f $gen_dir/VentureCore.Git.h ]] && cmp -s "$tmp" "$gen_dir/VentureCore.Git.h"; then
    rm -f "$tmp"
  else
    mv "$tmp" "$gen_dir/VentureCore.Git.h"
  fi
}

# ---------------------------------------------------------------------------
# Toolchain lock
# ---------------------------------------------------------------------------
#
# Every cl.exe / link.exe run must be serialised.  The build, the test runner and
# the load/ABI probes all drive the *same* Wine prefix, and MSVC keeps its temp
# files inside it; two concurrent invocations that share those temp paths are the
# one thing that reliably produces a bogus compile failure (D8037/D8050 or a
# truncated object).  The lock file lives in the prefix, so separate checkouts
# (including git worktrees) that share one prefix serialise against each other.
#
# usage: vcs_lock_toolchain   (keeps an flock'd fd open until the script exits)
vcs_lock_toolchain() {
  local lock_dir lock_path fd
  if [[ -d ${VCS_WINEPREFIX:-} ]]; then
    lock_dir="$VCS_WINEPREFIX"
  else
    lock_dir="$VCS_ROOT/Release/IntDir"
    mkdir -p "$lock_dir" 2>/dev/null || true
  fi
  lock_path="$lock_dir/.toolchain.lock"

  # NB: no 2>/dev/null here - "exec" with only redirections applies them to the
  # shell permanently, so redirecting stderr would silence every later message.
  if ! exec {fd}>"$lock_path"; then
    vcs_warn "cannot create the toolchain lock at $lock_path - continuing without serialisation"
    return 0
  fi
  if command -v flock >/dev/null 2>&1; then
    if ! flock -w 3600 "$fd"; then
      vcs_die "another build/test still holds the Wine toolchain lock after 1 h"
    fi
  fi
  VCS_LOCK_FD="$fd"
}
