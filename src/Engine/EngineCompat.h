#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// Compile-time half of the engine binding gate.
//
// The platform compiles against a pinned YRpp revision (src/Core/EnginePin.h);
// scripts/linux/check_yrpp.py is the build-time half. This header is the
// compile-time half: it includes the pinned engine wrapper and asserts the ABI
// surface the base and its features actually touch. Wherever a new engine type
// or field is used, add an assertion here as well - a YRpp update that silently
// moves a field must fail the build, not corrupt a savegame at run time.
//
// Values below were measured with scripts/linux/abi_probe.sh against the pinned
// YRpp revision. They belong to the pin: update them together with the pin and
// document the review in docs/EngineBindings.md.
// ---------------------------------------------------------------------------

#include <YRPP.h>
#include <EventClass.h>
#include <Helpers/Macro.h>

#include "Core/EnginePin.h"

#include <cstddef>

// gamemd.exe is a 32-bit process; a 64-bit platform DLL can never be loaded.
static_assert(sizeof(void*) == 4, "VentureCore must be built as a 32-bit (Win32/x86) DLL");

// Engine class sizes (YRpp c6c522f77b58508588d9a8652c53356e4aef7a96).
static_assert(sizeof(AbstractClass) == 0x24, "AbstractClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(ObjectClass) == 0xAC, "ObjectClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(FootClass) == 0x6C0, "FootClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(TechnoClass) == 0x520, "TechnoClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(UnitClass) == 0x8E8, "UnitClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(BulletClass) == 0x160, "BulletClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(BulletTypeClass) == 0x2F8, "BulletTypeClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(UnitTypeClass) == 0xE78, "UnitTypeClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(WeaponTypeClass) == 0x160, "WeaponTypeClass ABI changed - re-check the YRpp pin");

// INI layer: the platform walks the engine's parsed INI objects (rules-INI
// takeover), so the container layouts are part of the binding.
static_assert(sizeof(INIClass) == 0x40, "INIClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(CCINIClass) == 0x58, "CCINIClass ABI changed - re-check the YRpp pin");
static_assert(sizeof(INIClass::INIEntry) == 0x28, "INIEntry ABI changed - re-check the YRpp pin");
static_assert(sizeof(INIClass::INISection) == 0x44, "INISection ABI changed - re-check the YRpp pin");

// Network event record: the platform's event channel writes its payload into the
// engine's 104-byte data union, so the record layout is part of the binding.
// (The payload starts at offset 7: type + executed flag + house index + frame.)
static_assert(sizeof(EventClass) == 111, "EventClass ABI changed - re-check the YRpp pin");

// Syringe register frame layout (Syringe.h, SYR_VER=2). A wrong sized frame
// silently corrupts every hook handler.
static_assert(sizeof(REGISTERS) == 0x28, "Syringe REGISTERS ABI changed - check the SyringeEx version");
