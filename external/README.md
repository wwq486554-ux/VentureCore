# external/ - build dependencies, not vendored

Nothing in this directory is committed, except `YRpp.sha256` - the content
manifest of the pinned YRpp tree (sha256 hashes only, no YRpp content).  It
holds the engine bindings the platform compiles against.

## YRpp

`external/YRpp/` is the YRpp engine wrapper (headers + `StaticInits.cpp`) that
provides the C++ view of `gamemd.exe`. It is pinned by commit in
[`../src/Core/EnginePin.h`](../src/Core/EnginePin.h) (`VC_YRPP_PIN`).

YRpp does not carry a license declaration, so it is **not** redistributed with
this repository - it is an external build dependency (`NOTICE.md`).

Fetch it with a local checkout:

```bash
scripts/linux/fetch_yrpp.sh                 # finds a checkout on this machine
scripts/linux/fetch_yrpp.sh --from /path/to/YRpp
```

The script copies the headers into `external/YRpp/`, drops the upstream `.git`,
writes `external/YRpp.commit` and verifies the copy against
`external/YRpp.sha256` before swapping it in. `scripts/linux/check_yrpp.py`
(run by the build script) checks both the commit and the content manifest, and
refuses a dirty checkout.  When the pin changes, regenerate the manifest with
`fetch_yrpp.sh --write-manifest` and commit it; a non-git source tree is only
accepted when its content matches that manifest.
