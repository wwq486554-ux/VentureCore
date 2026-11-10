#pragma once

// ---------------------------------------------------------------------------
// Platform identity.
//
// This is the one header a developer edits to turn the VentureCore base into
// their own platform: change the names below (and ProjectName in
// VentureCore.vcxproj, which is what produces the DLL file name), rebuild, and
// the DLL / INI / log / version resource are all renamed with it.
//
// Rules:
//   * VC_PLATFORM_NAME is the human-facing platform name; it must also be a
//     valid file-name fragment (it becomes the INI and log file names).
//   * VC_SAVEGAME_ID identifies the platform's savegame trailer (format version
//     lives separately). Keep it unique per platform lineage.
//   * The feature switch section is one switch per feature system, see
//     docs/MakeYourOwnPlatform.md.
// ---------------------------------------------------------------------------

#define VC_PLATFORM_NAME     "VentureCore"
#define VC_PLATFORM_DLL_NAME VC_PLATFORM_NAME ".dll"
#define VC_PLATFORM_INI_NAME VC_PLATFORM_NAME ".ini"
#define VC_PLATFORM_LOG_NAME VC_PLATFORM_NAME ".log"

// 'V' 'C' '0' '1' - written into the savegame trailer header once save support
// lands. Unused in v0.2.0-experimental.
#define VC_SAVEGAME_ID 0x56433031u

// INI sections.
#define VC_SECTION_PLATFORM "Platform"
#define VC_SECTION_FEATURES "Features"

// Feature switches default to on: the platform is meant to be usable by
// dropping the DLL in and changing nothing else.
#define VC_DEFAULT_FEATURE_ENABLED true

// Directory of the running gamemd.exe is detected at run time; the INI and the
// log live next to rules.ini in the game directory.
