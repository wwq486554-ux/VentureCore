#!/usr/bin/env bash
# Print the engine/syringe ABI values that src/Engine/EngineCompat.h pins with
# static_asserts. Run this after updating the YRpp pin (see docs/EngineBindings.md)
# and copy the numbers into EngineCompat.h.
#
#   scripts/linux/abi_probe.sh
#
# Requires the same Wine/MSVC environment as the normal build; the probe is
# compiled into Release/IntDir/linux/abi-probe/ (build output, not committed).

set -euo pipefail

VCS_LINUX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$VCS_LINUX_DIR/common.sh"

command -v wine >/dev/null 2>&1 || vcs_die "wine is not installed"

vcs_locate_toolchain
vcs_wine_env

work_dir="$VCS_ROOT/Release/IntDir/linux/abi-probe"
mkdir -p "$work_dir"
probe_src="$work_dir/abi_probe.cpp"
probe_obj="$work_dir/abi_probe.obj"
probe_exe="$work_dir/abi_probe.exe"

cat > "$probe_src" <<'EOF'
#include <windows.h>
#include <YRPP.h>
#include <cstdio>

#define SHOW(type) printf("sizeof(%s) = 0x%X\n", #type, static_cast<unsigned>(sizeof(type)))

int __cdecl main()
{
	SHOW(AbstractClass);
	SHOW(AbstractTypeClass);
	SHOW(ObjectClass);
	SHOW(FootClass);
	SHOW(TechnoClass);
	SHOW(TechnoTypeClass);
	SHOW(UnitClass);
	SHOW(UnitTypeClass);
	SHOW(BulletClass);
	SHOW(BulletTypeClass);
	SHOW(WeaponTypeClass);
	SHOW(WarheadTypeClass);
	SHOW(RulesClass);
	SHOW(ScenarioClass);
	SHOW(CCINIClass);

	SHOW(REGISTERS);
	return 0;
}
EOF

cl_flags=(
  /nologo /c /W3 /std:c++20 /permissive- /MT /GR- /GS- /Gz /arch:SSE2 /fp:precise
  /DSYR_VER=2 /DHAS_EXCEPTIONS=0 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
  /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000
)

cd "$work_dir"
wine "$VCS_VC_BIN/cl.exe" "${cl_flags[@]}" "$(vcs_w "$probe_src")" "/Fo$(vcs_w "$probe_obj")"
wine "$VCS_VC_BIN/link.exe" /nologo "/OUT:$(vcs_w "$probe_exe")" /SUBSYSTEM:CONSOLE "/MACHINE:X86" "$(vcs_w "$probe_obj")"

echo "--- ABI probe (pinned YRpp) ---"
wine "$probe_exe"
