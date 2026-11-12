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

// 'V' 'C' '0' '1' - written into the savegame trailer payload header by
// SaveTrailer::Write (see src/Core/SaveTrailer.h, docs/SaveFormat.md); a trailer
// written under another identity is rejected as a whole.  Change it only for a
// new lineage.
#define VC_SAVEGAME_ID 0x56433031u

// INI sections.
#define VC_SECTION_PLATFORM "Platform"
#define VC_SECTION_FEATURES "Features"
#define VC_SECTION_GROUPS "Groups"

// Historical name of the "feature switches are on by default" policy.  The
// effective default is FeatureDescriptor::defaultEnabled (on unless a module
// overrides it); see include/platform/Feature.h.  Kept so existing derived
// platforms that reference the policy name keep compiling.
#define VC_DEFAULT_FEATURE_ENABLED true

// Directory of the running gamemd.exe is detected at run time; the INI and the
// log live next to rules.ini in the game directory.
