#pragma once

// ---------------------------------------------------------------------------
// Engine bindings pin (single source of truth).
//
// This platform compiles against a *specific* YRpp revision. YRpp has no
// license declaration and is not vendored in this repository; it is fetched
// into external/YRpp as an external build dependency (see external/README.md).
//
// Three gates enforce the pin:
//   1. build time  - scripts/linux/check_yrpp.py compares the local revision
//                    with VC_YRPP_PIN and aborts the build on a mismatch;
//   2. compile time - src/Engine/EngineCompat.h asserts the engine ABI that
//                    this revision provides;
//   3. run time    - the platform log prints the pin at startup, and the DLL
//                    version resource records it under "YrppCommit".
//
// Updating the pin is a compatibility review, not a version bump: see
// docs/EngineBindings.md.
// ---------------------------------------------------------------------------

#define VC_YRPP_REPO   "https://github.com/Phobos-developers/YRpp"
#define VC_YRPP_BRANCH "new_blitters"
#define VC_YRPP_PIN    "c6c522f77b58508588d9a8652c53356e4aef7a96"
#define VC_YRPP_PIN_SHORT "c6c522f7"
