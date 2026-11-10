# external/ - build dependencies, not vendored

Nothing in this directory is committed. It holds the engine bindings the
platform compiles against.

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
and writes `external/YRpp.commit` so `scripts/linux/check_yrpp.py` (run by the
build script) can verify the revision. When the repository is public, switch to
a git submodule; `check_yrpp.py` accepts both forms.
