#!/usr/bin/env bash
# Load the built platform DLL into a tiny Win32 host under Wine and resolve its
# hook entry points. This is the strongest check that can be done on this machine
# without starting the game: it exercises the PE image, the import table, the CRT
# static initializers and the exported SyringeEx handlers.
#
#   scripts/linux/load_probe.sh [Release/VentureCore.dll]
#
# The probe also verifies the lineage marker exports (VentureLineage_GetInfo /
# VentureLineage_CountConflicts) and the real mutual-exclusion path: it copies the
# DLL to a second file name, loads both copies into its own process, and requires
# the running copy to report at least one conflicting module - a copy of the same
# lineage is exactly what the platform must detect at startup (see
# docs/Lineage.md).
#
# The real in-game test (SyringeEx + gamemd.exe) still has to be run by a human
# on a machine with a writable game directory - see docs/HookCatalogue.md and
# README.md.

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$VCS_LINUX_DIR/common.sh"

command -v wine >/dev/null 2>&1 || vcs_die "wine is not installed"

target_name="$(sed -n 's:.*<ProjectName>\([^<]*\)</ProjectName>.*:\1:p' "$VCS_ROOT/VentureCore.vcxproj" | head -n 1)"
[[ -n $target_name ]] || vcs_die "cannot read ProjectName from VentureCore.vcxproj"

dll="${1:-$VCS_ROOT/Release/$target_name.dll}"
[[ -f $dll ]] || vcs_die "no platform DLL at '$dll' (build it first)"

vcs_locate_toolchain
vcs_lock_toolchain
vcs_wine_env

work_dir="$VCS_ROOT/Release/IntDir/linux/load-probe"
mkdir -p "$work_dir"
probe_src="$work_dir/load_probe.cpp"
probe_exe="$work_dir/load_probe.exe"

# Second copy of the same image under a different file name: loading it next to
# the original is the conflict the lineage scan must detect.
copy_dll="$work_dir/$(basename "${dll%.dll}")Probe.dll"

cat > "$probe_src" <<'EOF'
#include <windows.h>
#include <cstdio>

#include <platform/Lineage.h>

namespace
{
// Resolves an export to a typed pointer. The intermediate void* keeps the cast
// between the two function-pointer types explicit (FARPROC is stdcall).
template <typename TFunction>
TFunction Resolve(HMODULE module, const char* const name)
{
	return reinterpret_cast<TFunction>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// The savegame trailer id the marker must carry (VC_SAVEGAME_ID in
// src/Core/PlatformConfig.h; the probe hard-codes the ABI value on purpose).
constexpr unsigned kExpectedSavegameId = 0x56433031u;
}

int __cdecl main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: load_probe <platform.dll> <second-copy-of-platform.dll>\n");
		return 2;
	}

	HMODULE module = LoadLibraryA(argv[1]);
	std::printf("LoadLibraryA(\"%s\") = %p (GetLastError=%lu)\n", argv[1], static_cast<void*>(module), GetLastError());
	if (!module)
		return 1;

	static const char* const kExports[] = {
		"Venture_ExeRun",
		"Venture_ScenarioClear",
		"Venture_AnnounceInvalidPointer",
		"Venture_Example_UnitClass_CTOR",
		"Venture_Example_UnitClass_DTOR",
		VENTURE_LINEAGE_EXPORT_GET_INFO,
		VENTURE_LINEAGE_EXPORT_COUNT_CONFLICTS,
	};
	constexpr int kExpectedExports = static_cast<int>(sizeof(kExports) / sizeof(kExports[0]));

	int found = 0;
	for (const char* const name : kExports)
	{
		const void* const fn = reinterpret_cast<const void*>(GetProcAddress(module, name));
		std::printf("  export %-38s = %p\n", name, fn);
		if (fn)
			++found;
	}

	int failures = 0;

	// -- lineage marker ------------------------------------------------------
	const VentureLineageGetInfoFn getInfo = Resolve<VentureLineageGetInfoFn>(module, VENTURE_LINEAGE_EXPORT_GET_INFO);
	const VentureLineageCountConflictsFn countConflicts =
		Resolve<VentureLineageCountConflictsFn>(module, VENTURE_LINEAGE_EXPORT_COUNT_CONFLICTS);

	if (!getInfo || !countConflicts)
	{
		std::printf("lineage marker: FAIL (marker exports not resolved)\n");
		++failures;
	}
	else
	{
		const VentureLineageInfo* const info = getInfo();
		if (!info)
		{
			std::printf("lineage marker: FAIL (GetInfo returned null)\n");
			++failures;
		}
		else
		{
			std::printf(
				"lineage marker: magic=0x%08X abi=%u name=%s version=%s savegame=0x%08X\n",
				info->magic,
				info->abiVersion,
				info->name ? info->name : "<null>",
				info->version ? info->version : "<null>",
				info->savegameId);

			const bool ok = info->magic == VENTURE_LINEAGE_MAGIC
				&& info->abiVersion == VENTURE_LINEAGE_ABI_VERSION
				&& info->name != nullptr && info->name[0] != '\0'
				&& info->version != nullptr && info->version[0] != '\0'
				&& info->savegameId == kExpectedSavegameId;
			std::printf("lineage marker: %s\n", ok ? "PASS" : "FAIL");
			if (!ok)
				++failures;
		}
	}

	// -- real mutual exclusion: a second copy must be detected ---------------
	if (!countConflicts)
	{
		std::printf("lineage conflict test: SKIP (CountConflicts not resolved)\n");
	}
	else
	{
		const unsigned before = countConflicts();

		HMODULE copy = LoadLibraryA(argv[2]);
		std::printf("LoadLibraryA copy(\"%s\") = %p (GetLastError=%lu)\n", argv[2], static_cast<void*>(copy), GetLastError());

		unsigned withCopy = 0;
		unsigned afterFree = 0;
		if (copy)
		{
			withCopy = countConflicts();
			std::printf("lineage conflict test: modules before=%u, with second copy=%u\n", before, withCopy);
			FreeLibrary(copy);
			afterFree = countConflicts();
			std::printf("lineage conflict test: after FreeLibrary=%u\n", afterFree);
		}

		const bool ok = copy && before == 0 && withCopy >= 1 && afterFree == 0;
		std::printf("lineage conflict test: %s\n", ok ? "PASS" : "FAIL");
		if (!ok)
			++failures;
	}

	std::printf("FreeLibrary: %s\n", FreeLibrary(module) ? "ok" : "failed");
	std::printf("resolved exports: %d/%d\n", found, kExpectedExports);

	const bool ok = found == kExpectedExports && failures == 0;
	std::printf("load probe: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
EOF

cl_flags=(
  /nologo /W4 /MT /EHsc- /Gz /std:c++20
  /DSYR_VER=2 /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000
)

# Compile inside the work dir: cl.exe writes the object file next to its current
# directory, and a probe run from the repository root must not litter it.
( cd "$work_dir" && wine "$VCS_VC_BIN/cl.exe" "${cl_flags[@]}" "$(vcs_w "$probe_src")" \
    "/Fe$(vcs_w "$probe_exe")" /link /SUBSYSTEM:CONSOLE /MACHINE:X86 dbghelp.lib onecore.lib ) \
  >"$work_dir/build.log" 2>&1 \
  || { cat "$work_dir/build.log" >&2; vcs_die "load probe failed to build"; }

cp -f "$dll" "$copy_dll"
echo '--- load probe ---'
wine "$(vcs_w "$probe_exe")" "$(vcs_w "$dll")" "$(vcs_w "$copy_dll")"
echo '--- load probe OK ---'
