#!/usr/bin/env bash
# Creates (and validates) the Wine prefix used to run the real MSVC toolchain.
#
#   scripts/linux/setup_wineprefix.sh
#
# Environment (see common.sh): VCS_WINEPREFIX, VCS_MSVC_VER, VCS_SDK_VER,
# VCS_WINDOWS_ROOT.
#
# The Wine prefix MUST be on a Linux-native filesystem - see common.sh.

set -euo pipefail

# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[[ $(uname -s) == Linux ]] || vcs_die "this script is for Linux"
command -v wine >/dev/null 2>&1 || vcs_die "wine is not installed"

vcs_locate_toolchain
vcs_check_prefix_fs

# Creating/updating the prefix runs wineboot and cl.exe in the shared Wine
# prefix, so it must serialise with builds/tests/probes exactly like they do.
# The lock file path is fixed (inside the prefix) whether or not the prefix
# exists yet (TOOL-10).
vcs_lock_toolchain

vcs_info "Wine prefix : $VCS_WINEPREFIX"
vcs_info "MSVC        : $VCS_MSVC_DIR"
vcs_info "Windows SDK : $VCS_SDK_VER ($VCS_SDK_ROOT)"

if vcs_wine_ready; then
  vcs_info "prefix already initialised"
else
  vcs_info "creating prefix (this takes a moment) ..."
  mkdir -p "$VCS_WINEPREFIX"
  vcs_wine_env
  set +e
  vcs_wine_run "$VCS_SETUP_TIMEOUT" wineboot -u >/dev/null 2>&1
  wineboot_rc=$?
  set -e
  if (( wineboot_rc == 124 )); then
    vcs_die "wineboot timed out after ${VCS_SETUP_TIMEOUT}s (VCS_SETUP_TIMEOUT)"
  elif (( wineboot_rc != 0 )); then
    vcs_warn "wineboot returned $wineboot_rc; continuing anyway"
  fi
  vcs_wine_ready || vcs_die "prefix was not created correctly at $VCS_WINEPREFIX"
fi

vcs_wine_env
mkdir -p "$VCS_WINEPREFIX/drive_c/venturecore-tmp"

# cl.exe ships its message resources per language; 2052 = Simplified Chinese.
vcs_tool_is_localized() {
  local d
  for d in "$VCS_VC_BIN" "$VCS_VS_ROOT/Common7/IDE"; do
    [[ -d "$d/2052" ]] && return 0
  done
  return 1
}
if vcs_tool_is_localized; then
  if [[ -n ${VCS_WINE_LOCALE:-} ]]; then
    vcs_info "message locale: $VCS_WINE_LOCALE (keeps localised MSVC messages readable)"
  else
    vcs_warn "MSVC is a localised (Chinese) install but this host has no zh_CN locale,"
    vcs_warn "so its non-ASCII messages will show as '?'. Error codes, file names and"
    vcs_warn "line numbers stay readable; install a zh_CN.UTF-8 locale (or set"
    vcs_warn "VCS_WINE_LOCALE) if you want the full text."
  fi
fi

# ---------------------------------------------------------------------------
# Smoke test: compile a translation unit with the real cl.exe under Wine.
# ---------------------------------------------------------------------------
probe_dir="$VCS_WINEPREFIX/drive_c/venturecore-tmp"
probe_log="$probe_dir/probe.log"
printf 'extern "C" int vcs_probe(void) { return 42; }\n' > "$probe_dir/probe.cpp"
rm -f "$probe_dir/probe.obj"

set +e
vcs_wine_run "$VCS_COMPILE_TIMEOUT" wine "$VCS_VC_BIN/cl.exe" /nologo /c 'C:\venturecore-tmp\probe.cpp' \
  /Fo'C:\venturecore-tmp\probe.obj' >"$probe_log" 2>&1
rc=$?
set -e

# cl.exe is localised (GBK/CP936 messages on a Chinese install); make the log
# readable if it is not already UTF-8.
show_log() {
  if iconv -f UTF-8 -t UTF-8 "$1" >/dev/null 2>&1; then
    grep -v 'MESA' "$1" | tail -n 15
  else
    iconv -f GBK -t UTF-8 "$1" 2>/dev/null | grep -v 'MESA' | tail -n 15 || tail -n 15 "$1"
  fi
}

if (( rc == 124 )); then
  vcs_die "cl.exe smoke test timed out after ${VCS_COMPILE_TIMEOUT}s (VCS_COMPILE_TIMEOUT)"
fi
if [[ $rc -ne 0 || ! -f "$probe_dir/probe.obj" ]]; then
  vcs_warn "cl.exe smoke test failed (exit $rc):"
  show_log "$probe_log" >&2
  vcs_die "the toolchain does not run under Wine; see the message above (D8037 usually means the prefix is not on a native filesystem)"
fi

vcs_info "smoke test OK - cl.exe compiled a test unit"
vcs_info "done. Now run: scripts/linux/build_linux.sh Release"
