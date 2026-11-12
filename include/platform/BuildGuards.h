#pragma once

// ---------------------------------------------------------------------------
// Build invariants shared by every translation unit of the platform (TOOL-02).
//
// The platform is compiled with C++ exceptions *disabled*:
//   * neither the MSBuild project (VentureCore.props: ExceptionHandling=false,
//     which emits no /EH switch at all) nor the Wine build (scripts/linux/
//     common.sh: vcs_cl_flags) passes /EH;
//   * _HAS_EXCEPTIONS=0 is defined on the command line, so the CRT and the STL
//     abort instead of throwing (MSVC's vcruntime.h otherwise defaults the
//     macro to 1, and the exception machinery would be compiled back in the
//     moment anything throws).
//
// This header is force-included (/FI) into every translation unit: by
// VentureCore.props (ForcedIncludeFiles) for the Visual Studio build and by
// vcs_cl_flags for the DLL build, the unit tests, the benchmark and the probes.
// It turns a future configuration slip into a compile error instead of a
// silently different exception model:
//   * _CPPUNWIND is predefined by MSVC only when /GX, /clr or /EH is on
//     (Microsoft "Predefined macros"): if it is defined, /EH came back;
//   * _HAS_EXCEPTIONS is defined to 0 by both build systems; anything else
//     means the define was dropped or overridden.
//
// Keep this header free of includes and of any other content: it is compiled
// first in every translation unit and must never influence declarations.
// ---------------------------------------------------------------------------

#if defined(_CPPUNWIND)
#error "C++ exceptions are enabled (_CPPUNWIND is defined): build with no /EH option and _HAS_EXCEPTIONS=0 (see VentureCore.props / scripts/linux/common.sh)"
#endif

#if !defined(_HAS_EXCEPTIONS)
#error "_HAS_EXCEPTIONS is not defined: pass /D_HAS_EXCEPTIONS=0 (see VentureCore.props / scripts/linux/common.sh)"
#elif _HAS_EXCEPTIONS != 0
#error "_HAS_EXCEPTIONS must be 0: the platform is built without C++ exceptions"
#endif
