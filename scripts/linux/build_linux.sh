#!/usr/bin/env bash
# Builds VentureCore.dll on Linux by running the real MSVC toolchain under Wine.
#
#   scripts/linux/build_linux.sh [Debug|Release] [options]
#
# Options:
#   --build-type NIGHTLY|RELEASE   defines NIGHTLY / RELEASE (default: none, local build)
#   -j, --jobs N                   parallel compiler invocations (default: nproc)
#   --clean                        drop all Linux-side build state and rebuild
#   --check                        verify that VentureCore.props/.vcxproj and
#                                  the Linux compiler flags agree, print the
#                                  flag hash and exit (no Wine, no build)
#   --fix-eol                      normalise the line endings of repository text
#                                  files to what .gitattributes declares (CRLF),
#                                  then build. Without it the check only warns.
#   --stage                        also copy gamemd.edb (and Syringe.exe if found)
#   --deploy DIR                   copy VentureCore.dll / VentureCore.pdb / gamemd.edb into
#                                  DIR (the game directory). Existing files are
#                                  kept as *.prev first. VCS_DEPLOY_DIR sets the
#                                  default, so plain "build_linux.sh Release"
#                                  deploys as well.
#   -v, --verbose                  echo every compiler command; print warnings
#   --msvc-ver VER                 MSVC toolset to use (default 14.44.35207)
#   --sdk-ver VER                  Windows SDK version (default: newest installed)
#   --prefix PATH                  Wine prefix (default $HOME/.cache/venturecore-wine,
#                                  or <repo>/../.wine-venturecore when $HOME is not writable)
#   -h, --help
#
# The cl.exe options themselves live in scripts/linux/common.sh (vcs_cl_flags) -
# the single definition shared with run_tests.sh, bench_store.sh and the probes.
# The linker/resource-compiler command lines mirror what MSBuild produces for
# VentureCore.vcxproj.  The deliberate differences from the MSBuild build are
# listed in scripts/linux/README.md ("Intentional differences from the MSBuild
# build") and next to the flags in common.sh; nothing else is meant to differ.
#
# Diagnostics: the number of compiler/linker/resource warnings is counted, the
# summary is printed and archived as Release/IntDir/linux/log/build-summary.log,
# and VCS_WARNINGS_AS_ERRORS=1 turns the first warning into a build failure
# (TOOL-07).
#
# Exit status: 0 = DLL built (or --check passed), non-zero = failure.

set -euo pipefail

SELF_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() { awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$SELF_PATH"; }

cfg="Release"
build_type=""
do_clean=0
do_stage=0
do_fix_eol=0
do_check=0
deploy_dir=""
verbose=0
compile_one=""

while (( $# )); do
  case "$1" in
    Debug|debug)      cfg=Debug ;;
    Release|release)  cfg=Release ;;
    --build-type)     build_type="${2:?--build-type needs a value}"; shift ;;
    --build-type=*)   build_type="${1#*=}" ;;
    -j|--jobs)        VCS_JOBS="${2:?-j needs a value}"; shift ;;
    -j*)              VCS_JOBS="${1#-j}" ;;
    --clean)          do_clean=1 ;;
    --check)          do_check=1 ;;
    --stage)          do_stage=1 ;;
    --fix-eol)        do_fix_eol=1 ;;
    --deploy)         deploy_dir="${2:?--deploy needs a directory}"; shift ;;
    --deploy=*)       deploy_dir="${1#*=}" ;;
    -v|--verbose)     verbose=1 ;;
    --msvc-ver)       VCS_MSVC_VER="${2:?}"; shift ;;
    --sdk-ver)        VCS_SDK_VER="${2:?}"; shift ;;
    --prefix)         VCS_WINEPREFIX="${2:?}"; shift ;;
    --compile-one)    compile_one="${2:?}"; shift ;;
    -h|--help)        usage; exit 0 ;;
    *)                vcs_die "unknown argument: $1 (try --help)" ;;
  esac
  shift
done

[[ $build_type == "" || $build_type == NIGHTLY || $build_type == RELEASE ]] \
  || vcs_die "--build-type must be NIGHTLY or RELEASE"
[[ $VCS_JOBS =~ ^[0-9]+$ && $VCS_JOBS -gt 0 ]] || vcs_die "-j needs a positive integer"
deploy_dir="${deploy_dir:-${VCS_DEPLOY_DIR:-}}"

# The --compile-one child re-executes this script, but its argv carries only the
# compile job - the configuration has to come from the environment the parent
# exported. Without this the child would fall back to the default configuration
# and write its object files into the wrong configuration's tree.
if [[ -n $compile_one ]]; then
  cfg="${VCS_CHILD_CFG:?child job error: VCS_CHILD_CFG is not set}"
  build_type="${VCS_CHILD_BT:-}"
fi

command -v wine >/dev/null 2>&1 || vcs_die "wine is not installed"
vcs_locate_toolchain

# Engine binding gate: refuse to build against an unknown YRpp revision. The
# source of truth is src/Core/EnginePin.h; see docs/EngineBindings.md.
if [[ -z $compile_one ]]; then
  python3 "$VCS_ROOT/scripts/linux/check_yrpp.py" \
    || vcs_die "YRpp pin check failed - run scripts/linux/fetch_yrpp.sh to (re)fetch the pinned YRpp"
fi

# ---------------------------------------------------------------------------
# Derived paths (kept identical in the --compile-one child)
# ---------------------------------------------------------------------------
# The output file name is the project name, exactly like MSBuild: renaming the
# platform means editing <ProjectName> in VentureCore.vcxproj (and the strings in
# src/Core/PlatformConfig.h), not this script. See docs/Renaming.md.
target_name="$(sed -n 's:.*<ProjectName>\([^<]*\)</ProjectName>.*:\1:p' "$VCS_ROOT/VentureCore.vcxproj" | head -n 1)"
[[ -n $target_name ]] || vcs_die "cannot read ProjectName from VentureCore.vcxproj"

out_dir="$VCS_ROOT/$cfg"
int_dir="$out_dir/IntDir"
work_dir="$int_dir/linux"        # Linux-side build state (leaves MSBuild's own IntDir alone)
obj_dir="$work_dir/obj"
log_dir="$work_dir/log"
fail_dir="$work_dir/failed"
scratch_dir="$work_dir/scratch"
gen_dir="$int_dir/Generated"     # same location MSBuild's ComputeGitInfo target uses
dll_path="$out_dir/$target_name.dll"
pdb_path="$out_dir/$target_name.pdb"
implib_path="$int_dir/$target_name.lib"

# Export the settings the child processes need; they must resolve identically.
export VCS_WINEPREFIX VCS_MSVC_VER VCS_SDK_VER VCS_WINDOWS_ROOT VCS_JOBS
export VCS_DEBUG_INFO VCS_MODULES VCS_CHILD_CFG="$cfg" VCS_CHILD_BT="$build_type"
export VCS_COMPILE_TIMEOUT VCS_LINK_TIMEOUT
# /Zi needs a PDB path; the --compile-one child recomputes $int_dir itself, so
# this is only for callers inside this script and for the hash.
export VCS_PDB_DIR="$int_dir"

# ---- --check: static agreement check, no Wine (TOOL-01/TOOL-10) -------------
if (( do_check )); then
  vcs_check_project_sync
  vcs_assert_flag_parity "$cfg" "$build_type"
  flags_hash_check="$(vcs_flags_hash "$cfg" "$build_type" build)"
  vcs_info "project files and Linux build agree"
  vcs_info "compile flags : $flags_hash_check (single definition: scripts/linux/common.sh, vcs_cl_flags)"
  vcs_info "profile hashes: build=$flags_hash_check test=$(vcs_flags_hash "$cfg" "$build_type" test) bench=$(vcs_flags_hash "$cfg" "$build_type" bench) probe=$(vcs_flags_hash "$cfg" "$build_type" probe)"
  exit 0
fi

# Makes an MSVC log readable. cl.exe/rc.exe/link.exe are localised (CP936 on a
# Chinese install) while Wine's own diagnostics are ASCII - GBK covers both, and
# "-c" drops anything that still does not convert. grep runs with -a so that a
# log containing non-UTF-8 bytes is not silently reported as "binary file
# matches" instead of being shown.
vcs_show_log() {
  local log="$1" text
  [[ -f $log ]] || return 0
  if iconv -f UTF-8 -t UTF-8 "$log" >/dev/null 2>&1; then
    text="$(cat "$log")"
  else
    # "-c" is essential: without it a single unmappable byte would make iconv
    # fail and a naive fallback would print raw GBK bytes as mojibake.
    text="$(iconv -f GBK -t UTF-8 -c "$log" 2>/dev/null || true)"
    [[ -n $text ]] || text="$(cat "$log")"
  fi
  printf '%s\n' "$text" | grep -a -v 'MESA' | tail -n 25
}

# ---------------------------------------------------------------------------
# Child mode: compile exactly one translation unit.
# ---------------------------------------------------------------------------
if [[ -n $compile_one ]]; then
  IFS=$'\t' read -r src_rel obj_rel <<<"$compile_one"
  vcs_wine_env "$gen_dir"
  mkdir -p "$obj_dir/$(dirname "$obj_rel")" "$log_dir" "$scratch_dir"
  log="$log_dir/${src_rel//\//__}.log"
  printf '  CC %s\n' "$src_rel" >&2
  set +e
  # Compile from the repository root and pass the *relative* source path: /FC is
  # not used, so __FILE__ (asserts, the feature-registration diagnostics) then
  # records "src/Core/Log.cpp" instead of a Z:\home\... build-machine path.  The
  # object still goes to the absolute /Fo path, so nothing is written to the
  # working directory (TOOL-03/TOOL-08).
  cd "$VCS_ROOT"
  # shellcheck disable=SC2046  # flag list is intentionally word-split
  vcs_wine_run "$VCS_COMPILE_TIMEOUT" wine "$VCS_VC_BIN/cl.exe" /c \
    $(vcs_cl_flags "$VCS_CHILD_CFG" "$VCS_CHILD_BT" build) \
    "$src_rel" "/Fo$(vcs_w "$obj_dir/$obj_rel")" >"$log" 2>&1
  rc=$?
  set -e
  if (( rc != 0 )); then
    if (( rc == 124 )); then
      printf 'fatal error: cl.exe timed out after %ss (VCS_COMPILE_TIMEOUT)\n' "$VCS_COMPILE_TIMEOUT" >>"$log"
    fi
    mkdir -p "$fail_dir"
    printf '%s\n' "$src_rel" > "$fail_dir/${src_rel//\//__}"
    exit 1
  fi
  if [[ -s $log ]]; then
    filtered="$(vcs_show_log "$log")"
    if (( verbose )) || grep -qiE 'warning|error' <<<"$filtered"; then
      printf '%s\n' "$filtered" >&2
    fi
  fi
  exit 0
fi

# ---------------------------------------------------------------------------
# Main build
# ---------------------------------------------------------------------------
# Report any command that fails under "set -e" instead of exiting silently.
vcs_err_report() {
  printf '[linux-build] error: command failed (exit %d) at line %d: %s\n' "$1" "$2" "$3" >&2
}
trap 'vcs_err_report $? $LINENO "$BASH_COMMAND"' ERR

# Serialise against the test runner / probes and against other checkouts that
# share this Wine prefix (see common.sh). Never held in --compile-one children.
vcs_lock_toolchain

vcs_wine_env "$gen_dir"
vcs_wine_ready || vcs_die "Wine prefix '$VCS_WINEPREFIX' is not initialised - run scripts/linux/setup_wineprefix.sh first"

# The project files are the source of truth for what is compiled, but the
# include path and the batch-A build invariants (no C++ exceptions, NDEBUG in
# Release) are also mirrored here by hand - refuse to build when they disagree
# with VentureCore.props (TOOL-10).
vcs_check_project_sync

if (( do_clean )); then
  vcs_info "cleaning $work_dir"
  rm -rf "$work_dir"
  rm -f "$dll_path" "$pdb_path" "$implib_path"
fi
mkdir -p "$obj_dir" "$log_dir" "$scratch_dir" "$gen_dir"

# --- 0. line endings ---------------------------------------------------------
#
# .gitattributes declares `* text eol=crlf`. Git normalises on commit, so a file
# with the wrong working-copy line endings still commits cleanly and still
# compiles - the damage is invisible until someone notices a whole-file diff. The
# usual cause is editing a file with a tool that rewrites text without preserving
# line endings (Python's text-mode open() is the classic one; write with
# newline='\r\n').
#
# This is a WARNING, never an error: a stray LF does not break the build, and a
# build script must not fail for a cosmetic reason. Run --fix-eol to normalise.
if [[ -z $compile_one ]]; then
  if (( do_fix_eol )); then
    vcs_info "normalising line endings (--fix-eol)"
    python3 "$VCS_ROOT/scripts/linux/check_eol.py" --fix || true
  else
    if ! python3 "$VCS_ROOT/scripts/linux/check_eol.py"; then
      vcs_warn "line endings above differ from .gitattributes (harmless for the build);"
      vcs_warn "fix with: scripts/linux/build_linux.sh $cfg --fix-eol"
    fi
  fi
fi

# --- 1. generated version header (VentureCore.props' ComputeGitInfo target) ------
# One generator for every script: common.sh's vcs_generate_git_header (the copy
# that used to live here had drifted from it, TOOL-10).  It is silent when the
# header is unchanged; print the same message the old local copy printed.
git_header_before="$(cat "$gen_dir/VentureCore.Git.h" 2>/dev/null || true)"
vcs_generate_git_header "$gen_dir"
git_header_after="$(cat "$gen_dir/VentureCore.Git.h" 2>/dev/null || true)"
if [[ $git_header_before != "$git_header_after" ]]; then
  vcs_info "generated $gen_dir/VentureCore.Git.h"
fi

# --- 2. source list and incremental decision --------------------------------
mapfile -t sources < <(vcs_list_sources)
(( ${#sources[@]} > 0 )) || vcs_die "no sources found in VentureCore.vcxproj"
mapfile -t resources < <(vcs_list_resources)

missing=0
for s in "${sources[@]}"; do
  [[ -f $VCS_ROOT/$s ]] || { vcs_warn "listed in the project but missing on disk: $s"; missing=1; }
done
(( missing == 0 )) || vcs_die "the project references files that do not exist (was a file deleted without updating VentureCore.vcxproj?)"

# The project file is the single source of truth for what is compiled (MSBuild
# behaves the same).  The reverse direction has no such error path: a new
# src/**/*.cpp that was never added to VentureCore.vcxproj is silently not
# compiled - a copied-in feature module would just do nothing.  Warn once per
# file so that cannot happen without a message (D-12).
unlisted=0
while IFS= read -r disk_source; do
  [[ -n $disk_source ]] || continue
  if ! printf '%s\n' "${sources[@]}" | grep -Fxq "$disk_source"; then
    vcs_warn "source file exists but is not in VentureCore.vcxproj, so it is NOT compiled: $disk_source"
    unlisted=1
  fi
done < <(find "$VCS_ROOT/src" -type f -name '*.cpp' -printf 'src/%P\n' 2>/dev/null | sort)
if (( unlisted )); then
  vcs_warn "add the file(s) above to the <ClCompile> list in VentureCore.vcxproj, or delete them"
fi

cl_flags_str="$(vcs_cl_flags "$cfg" "$build_type" | tr '\n' ' ')"
# The toolset identity is part of the hash: switching MSVC/SDK versions must
# invalidate every object file.
flags_hash="$(printf '%s' "$cl_flags_str MSVC=$VCS_MSVC_VER SDK=$VCS_SDK_VER" | md5sum | cut -d' ' -f1)"
flags_file="$work_dir/flags.md5"
previous_hash="$(cat "$flags_file" 2>/dev/null || true)"
force_all=0
if [[ $previous_hash != "$flags_hash" ]]; then
  [[ -n $previous_hash ]] && vcs_info "compiler options changed - full rebuild"
  force_all=1
fi

# The project files are build inputs in their own right: the source/resource
# lists and the compiler include path come from VentureCore.vcxproj +
# VentureCore.props, and external/YRpp/YRpp.props contributes the YRpp sources.
# Their contents are also part of the link hash below.
project_files=(
  "$VCS_ROOT/VentureCore.vcxproj"
  "$VCS_ROOT/VentureCore.props"
  "$VCS_ROOT/external/YRpp/YRpp.props"
)

# Linker inputs. The command line is defined here, before the link decision, so
# that it can be hashed: changing /TLBID, /OPT, the library list or one of the
# project files must relink even when no object file changed (D-11).
link_args=(
  /NOLOGO /DLL /MACHINE:X86 /INCREMENTAL:NO
  "/OUT:$(vcs_w "$dll_path")" "/PDB:$(vcs_w "$pdb_path")" "/IMPLIB:$(vcs_w "$implib_path")"
  /DEBUG:FULL /MANIFEST:NO /SUBSYSTEM:WINDOWS
  /OPT:REF /OPT:ICF /TLBID:1 /DYNAMICBASE /NXCOMPAT /SAFESEH
  dbghelp.lib onecore.lib
)
link_hash="$(
  {
    printf 'link-args: %s\n' "${link_args[*]}"
    for f in "${project_files[@]}"; do
      [[ -f $f ]] || continue
      printf 'project-file: %s\n' "${f#"$VCS_ROOT/"}"
      md5sum "$f" | cut -d' ' -f1
    done
  } | md5sum | cut -d' ' -f1
)"
link_md5_file="$work_dir/link.md5"
previous_link_hash="$(cat "$link_md5_file" 2>/dev/null || true)"
link_inputs_changed=0
if [[ -n $previous_link_hash && $previous_link_hash != "$link_hash" ]]; then
  vcs_info "linker inputs changed - relink"
  link_inputs_changed=1
fi

# Newest header/resource in the project: any change to it invalidates every
# object (this build does not parse /showIncludes dependency files yet).
# include/ is a real build input (VentureCore.props puts it on the include path),
# so it has to be part of the stamp too - editing the public feature API used to
# recompile nothing (D-10).
# awk (not "sort | head") is used on purpose: a truncated pipeline would make
# the shell report a spurious failure under "set -o pipefail".
stamp="$work_dir/headers.stamp"
newest_header="$(find "$VCS_ROOT/src" "$VCS_ROOT/include" "$VCS_ROOT/external/YRpp" -type f \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.inl' -o -name '*.rc' \) \
  -printf '%T@ %p\n' 2>/dev/null | awk '
    { t = $1 + 0; sub(/^[^ ]+ /, ""); if (t > m) { m = t; newest = $0 } }
    END { if (newest != "") print newest }')"
if [[ -n $newest_header ]]; then
  touch -r "$newest_header" "$stamp"
fi
# A changed project file must also invalidate every object: the source list,
# include paths and resource list are read from it on the next build.
for f in "${project_files[@]}"; do
  [[ -f $f ]] || continue
  if [[ ! -f $stamp || $f -nt $stamp ]]; then
    touch -r "$f" "$stamp"
  fi
done
if [[ $gen_dir/VentureCore.Git.h -nt $stamp ]]; then
  touch -r "$gen_dir/VentureCore.Git.h" "$stamp"
fi

pending=()
for s in "${sources[@]}"; do
  obj="$obj_dir/$s.obj"
  if (( force_all )) || [[ ! -f $obj ]] || [[ ! -f $stamp ]] \
     || [[ $VCS_ROOT/$s -nt $obj ]] || [[ $stamp -nt $obj ]]; then
    pending+=("$s")
  fi
done

vcs_info "configuration : $cfg${build_type:+ / $build_type}"
vcs_info "toolchain     : MSVC $VCS_MSVC_VER + Windows SDK $VCS_SDK_VER (via $WINEPREFIX)"
vcs_info "sources       : ${#sources[@]} total, ${#pending[@]} to compile (jobs: $VCS_JOBS)"
vcs_info "flags hash    : $flags_hash (incremental invalidation; vcs_cl_flags in scripts/linux/common.sh)"
vcs_info "profile hash  : $(vcs_flags_hash "$cfg" "$build_type" build) (the tests/benchmark/probes must report the same hash)"
if [[ ${VCS_WARNINGS_AS_ERRORS:-0} == 1 ]]; then
  vcs_info "warnings      : fatal (/WX, VCS_WARNINGS_AS_ERRORS=1)"
fi

build_start=$(date +%s)

# --- 3. compile -------------------------------------------------------------
if (( ${#pending[@]} > 0 )); then
  rm -rf "$fail_dir"
  mkdir -p "$fail_dir"
  pending_file="$work_dir/pending.txt"
  : > "$pending_file"
  for s in "${pending[@]}"; do
    printf '%s\t%s.obj\n' "$s" "$s" >> "$pending_file"
  done
  # A non-zero exit here is expected when a translation unit fails; the failure
  # markers carry the details, so keep the ERR trap out of the way.
  set +e
  trap - ERR
  xargs -d '\n' -I '{}' -P "$VCS_JOBS" bash "$SELF_PATH" --compile-one '{}' < "$pending_file"
  xargs_rc=$?
  trap 'vcs_err_report $? $LINENO "$BASH_COMMAND"' ERR
  set -e
  if (( xargs_rc != 0 )) && [[ -z "$(ls -A "$fail_dir" 2>/dev/null)" ]]; then
    vcs_die "the compiler driver failed (xargs exit $xargs_rc) - inspect $log_dir"
  fi
  failed=$(find "$fail_dir" -type f 2>/dev/null | wc -l)
  if (( failed > 0 )); then
    for f in "$fail_dir"/*; do
      rel="$(cat "$f")"
      printf '\n===== %s\n' "$rel" >&2
      vcs_show_log "$log_dir/${rel//\//__}.log" >&2
    done
    vcs_die "$failed translation unit(s) failed to compile"
  fi
  vcs_info "compiled ${#pending[@]} translation unit(s)"
fi

# --- 4. resources -----------------------------------------------------------
#
# The resources must see the same build type as the sources. src/version.rc
# includes VentureCore.version.h, whose PRODUCT_VERSION / FILE_VERSION_STR differ
# between a plain local build and a NIGHTLY/RELEASE one (the latter two append
# PRERELEASE_SUFFIX). rc.exe only honours /d definitions, so the flag has to be
# repeated here - otherwise the DLL's StringFileInfo always reports a local
# build's version even when --build-type RELEASE was requested.
rc_defs=()
case "$build_type" in
  NIGHTLY) rc_defs+=( /dNIGHTLY ) ;;
  RELEASE) rc_defs+=( /dRELEASE ) ;;
esac

link_input=()
link_input_win=()
rc_logs=()
for r in "${resources[@]}"; do
  res="$int_dir/$(basename "${r%.rc}").res"
  # A .res bakes in the preprocessor state of the .rc it came from. Comparing it
  # against the .rc timestamp alone is not enough: switching --build-type does not
  # touch the .rc's timestamp but does change the version string inside, so a local
  # build followed by a RELEASE build would silently ship the local build's
  # FileVersion/ProductVersion metadata. force_all is set whenever the
  # compiler-option hash (which includes /DRELEASE or /DNIGHTLY) changes, so gate
  # the resources on it too. The .res lives in MSBuild's own IntDir, which this
  # script otherwise leaves alone, hence the explicit invalidation here.
  # The generated git header is part of the .rc's preprocessor input
  # (VentureCore.Git.h -> PlatformVersion.h -> version.rc), so a commit change
  # must invalidate the .res even though neither the .rc nor the compiler flags
  # changed. Without this the version resource would keep the previous stamp.
  if (( force_all )) || [[ ! -f $res || $VCS_ROOT/$r -nt $res ]] \
     || [[ -f $gen_dir/VentureCore.Git.h && $gen_dir/VentureCore.Git.h -nt $res ]]; then
    vcs_info "RC ${r}"
    rc_log="$log_dir/rc_$(basename "$r").log"
    set +e
    ( cd "$scratch_dir" && vcs_wine_run "$VCS_LINK_TIMEOUT" wine "$VCS_SDK_RC" \
        /l0x0409 /nologo "${rc_defs[@]+"${rc_defs[@]}"}" \
        "/fo$(vcs_w "$res")" "$(vcs_w "$VCS_ROOT/$r")" ) >"$rc_log" 2>&1
    rc_rc=$?
    set -e
    if (( rc_rc == 124 )); then
      printf 'fatal error: rc.exe timed out after %ss (VCS_LINK_TIMEOUT)\n' "$VCS_LINK_TIMEOUT" >>"$rc_log"
    fi
    if (( rc_rc != 0 )) || [[ ! -f $res ]]; then
      vcs_show_log "$rc_log" >&2
      vcs_die "resource compilation failed for $r (exit $rc_rc, see $rc_log)"
    fi
    rc_logs+=("$rc_log")
  fi
  link_input+=("$res"); link_input_win+=("$(vcs_w "$res")")
done

for s in "${sources[@]}"; do
  link_input+=("$obj_dir/$s.obj"); link_input_win+=("$(vcs_w "$obj_dir/$s.obj")")
done

# --- 5. link ----------------------------------------------------------------
needs_link=0
if [[ ! -f $dll_path ]]; then
  needs_link=1
elif [[ ! -f $link_md5_file ]]; then
  # No link state was recorded yet (first run after this script was updated):
  # relink once so the binary is known to match the current link inputs.
  needs_link=1
elif (( link_inputs_changed )); then
  needs_link=1
elif (( ${#pending[@]} > 0 )); then
  needs_link=1
else
  for f in "${link_input[@]}"; do
    [[ $f -nt $dll_path ]] && { needs_link=1; break; }
  done
fi

if (( needs_link )); then
  vcs_info "linking $dll_path"
  # Clear previous outputs first: link.exe refuses to overwrite a truncated or
  # corrupt import library (LNK1136) left behind by an interrupted build.
  rm -f "$dll_path" "$pdb_path" "$implib_path" "${implib_path%.lib}.exp"
  # Scratch is the working directory of the tools (rc.exe and link.exe drop
  # temporary/default-named files there).
  find "$scratch_dir" -mindepth 1 -delete 2>/dev/null || true
  link_log="$log_dir/link.log"
  set +e
  ( cd "$scratch_dir" && vcs_wine_run "$VCS_LINK_TIMEOUT" wine "$VCS_VC_BIN/link.exe" \
      "${link_args[@]}" "${link_input_win[@]}" ) >"$link_log" 2>&1
  link_rc=$?
  set -e
  if (( link_rc == 124 )); then
    printf 'fatal error: link.exe timed out after %ss (VCS_LINK_TIMEOUT)\n' "$VCS_LINK_TIMEOUT" >>"$link_log"
  fi
  if (( link_rc != 0 )) || [[ ! -f $dll_path ]]; then
    vcs_show_log "$link_log" >&2
    vcs_die "linking failed (exit $link_rc, see $link_log)"
  fi
  if grep -qiE 'warning|error' "$link_log"; then vcs_show_log "$link_log" >&2; fi
else
  vcs_info "up to date - nothing to link"
fi

# --- 5b. diagnostics summary and build transcript (TOOL-07) ------------------
#
# "0 warnings" used to be a human observation: the script printed the warning
# lines but never counted them, and /WX- made sure a warning could not fail the
# build.  Count every diagnostic of *this* run, print a machine-readable summary,
# archive a transcript next to the per-file logs (copied into the acceptance log
# set) and fail when VCS_WARNINGS_AS_ERRORS=1.  For an incremental run only the
# TUs/tools that actually ran are counted; the release acceptance runs --clean.
compiled_warnings=0
other_warnings=0
warning_lines=()
diagnostic_logs=()
for s in "${pending[@]}"; do
  diagnostic_logs+=("$log_dir/${s//\//__}.log")   # compiler output (all TU logs are created)
done
diagnostic_logs+=("${rc_logs[@]+"${rc_logs[@]}"}")
if (( needs_link )) && [[ -f $log_dir/link.log ]]; then
  diagnostic_logs+=("$log_dir/link.log")
fi
for diag_log in "${diagnostic_logs[@]}"; do
  [[ -f $diag_log ]] || continue
  tu_warnings="$(grep -acE 'warning C[0-9]+' "$diag_log" 2>/dev/null || true)"
  compiled_warnings=$((compiled_warnings + ${tu_warnings:-0}))
  tool_warnings="$(grep -acE 'warning (LNK|RC)[0-9]+' "$diag_log" 2>/dev/null || true)"
  other_warnings=$((other_warnings + ${tool_warnings:-0}))
  while IFS= read -r warning_line; do
    warning_lines+=("$warning_line")
  done < <(grep -aE 'warning (C|LNK|RC)[0-9]+' "$diag_log" 2>/dev/null | sed -n '1,5p')
done
total_warnings=$((compiled_warnings + other_warnings))
vcs_info "compiler diagnostics: $compiled_warnings warning(s) in ${#pending[@]} compiled translation unit(s)"
vcs_info "linker/resource diagnostics: $other_warnings warning(s)"
for warning_line in "${warning_lines[@]+"${warning_lines[@]}"}"; do
  printf '[linux-build]   %s\n' "$warning_line" >&2
done

transcript="$log_dir/build-summary.log"
{
  printf '# VentureCore Linux build transcript\n'
  printf 'generated=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'config=%s\n' "$cfg"
  printf 'build_type=%s\n' "${build_type:-LOCAL}"
  printf 'toolchain=MSVC %s, Windows SDK %s\n' "$VCS_MSVC_VER" "$VCS_SDK_VER"
  printf 'flags_hash=%s\n' "$flags_hash"
  printf 'profile_flags_hash=%s\n' "$(vcs_flags_hash "$cfg" "$build_type" build)"
  printf 'cl_flags=%s\n' "${cl_flags_str% }"
  printf 'warnings_as_errors=%s\n' "${VCS_WARNINGS_AS_ERRORS:-0}"
  printf 'sources_total=%s\n' "${#sources[@]}"
  printf 'sources_compiled=%s\n' "${#pending[@]}"
  printf 'resources_compiled=%s\n' "${#rc_logs[@]}"
  printf 'warnings_compile=%s\n' "$compiled_warnings"
  printf 'warnings_linker_resource=%s\n' "$other_warnings"
  printf 'warnings_total=%s\n' "$total_warnings"
  for warning_line in "${warning_lines[@]+"${warning_lines[@]}"}"; do
    printf 'warning=%s\n' "$warning_line"
  done
  printf 'dll=%s\n' "$dll_path"
  if [[ -f $dll_path ]]; then printf 'dll_size=%s\n' "$(stat -c %s "$dll_path")"; fi
  printf 'result=success\n'
} >"$transcript"
vcs_info "build transcript: $transcript"

if (( total_warnings > 0 )) && [[ ${VCS_WARNINGS_AS_ERRORS:-0} == 1 ]]; then
  vcs_die "$total_warnings toolchain warning(s); VCS_WARNINGS_AS_ERRORS=1 forbids warnings"
fi

# --- 6. staging (optional) --------------------------------------------------
if (( do_stage )); then
  [[ -f $VCS_ROOT/gamemd.edb ]] && cp -f "$VCS_ROOT/gamemd.edb" "$out_dir/gamemd.edb" \
    && vcs_info "staged gamemd.edb"
  syringe="${VCS_SYRINGE_EXE:-}"
  if [[ -n $syringe && -f $syringe ]]; then
    cp -f "$syringe" "$out_dir/Syringe.exe" && vcs_info "staged Syringe.exe"
  else
    vcs_warn "Syringe.exe not staged (set VCS_SYRINGE_EXE=/path/to/Syringe.exe to bundle it)"
  fi
fi

# --- 7. deploy (optional) ---------------------------------------------------
if [[ -n $deploy_dir ]]; then
  [[ -d $deploy_dir ]] || vcs_die "--deploy: '$deploy_dir' is not a directory"
  [[ -f $deploy_dir/gamemd.exe ]] || vcs_warn "'$deploy_dir' contains no gamemd.exe - is that really the game directory?"

  deploy_one() {
    local name="$1" src="$2"
    [[ -f $src ]] || { vcs_warn "cannot deploy $name: $src does not exist"; return 0; }
    if [[ -f $deploy_dir/$name ]] && cmp -s "$src" "$deploy_dir/$name"; then
      vcs_info "$name unchanged in $deploy_dir"
      return 0
    fi
    if [[ -f $deploy_dir/$name ]]; then
      cp -f "$deploy_dir/$name" "$deploy_dir/$name.prev"
      vcs_info "kept the previous $name as $name.prev"
    fi
    cp -f "$src" "$deploy_dir/$name"
    vcs_info "deployed $name -> $deploy_dir"
  }
  deploy_one "$target_name.dll" "$dll_path"
  deploy_one "$target_name.pdb" "$pdb_path"
  deploy_one gamemd.edb "$VCS_ROOT/gamemd.edb"
fi

printf '%s' "$flags_hash" > "$flags_file"
printf '%s\n' "$link_hash" > "$link_md5_file"

elapsed=$(( $(date +%s) - build_start ))
printf '\n'
vcs_info "done in ${elapsed}s"
ls -l "$dll_path" "$pdb_path" 2>/dev/null >&2 || true
vcs_info "output: $dll_path"
