# v2 research archive — 2026-09-12

Baseline: `9b603c6` (merge PR #6). `git fetch --all --tags --prune`
confirmed that PR #5 and `45b2e55` are already ancestors of `origin/main`.
`git log origin/main..codex/v2-multichannel` and the tree diff were empty.
No duplicate merge, force push, or protection bypass was needed.

The initial working tree was clean. Existing local `codex/v3` pointed at
`45b2e55` with no independent commits; both local main and v3 were fast-forwarded
to `9b603c6`. No local model, recording, or training asset was changed.

Annotated tag: `echoradar-v2-archive-20260912`, pointing to `9b603c6`.
This is a traceable research archive, **not a fully accepted stable release**.
The tag annotation records the test totals and unresolved gates; detailed
evidence is retained on the v3 branch under
[validation/v2-final-20260912](validation/v2-final-20260912).

| Check on baseline source | Result |
| --- | --- |
| Native Release build, ONNX enabled | Passed |
| Native Release ctest | 93/93 passed |
| Native Debug build, ONNX enabled | Passed |
| Native Debug ctest | 93/93 passed |
| Python `ml/.venv/Scripts/python.exe -m pytest ml/tests -q` | 31/31 passed |
| Release `--help`, endpoint enumeration | Passed |
| Default endpoint loopback, 0.5 s | Running, stereo 48 kHz, generation 1, no reported drops/discards; silence only |

Toolchain: MSVC 14.51.36231, Ninja Multi-Config, CMake 4.4.0-rc2;
ONNX Runtime native package 1.27.0. Binary hashes and complete test logs are
included. These tests establish build/regression health only.
Verbose native build logs are preserved in `native-build-logs.zip`; test logs
remain plain text. Console line-padding spaces were removed from the smoke log.

Still unmeasured: real CS2 event/direction accuracy, real discrete 5.1/7.1
independence, end-to-end HUD and listening latency, transfer latency/drift,
two-hour stability, hotplug/sleep/format-change matrix, callback allocation
instrumentation, all display/DPI and game-window/HUD checks. Historical
[v2 evaluation gates](evaluation.md) remain open.
