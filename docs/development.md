# Development

## Supported configurations

The application and audio monitor target Windows 10/11 x64. Portable audio,
DSP, settings, state, and test libraries can be built on other hosts. CMake
3.20+ and C++20 are required. The first configure fetches pinned dependencies.

Key options:

- `ECHORADAR_BUILD_APP=ON`: build `EchoRadarV2.exe` on Windows.
- `ECHORADAR_BUILD_AUDIO_MONITOR=ON`: build the loopback monitor.
- `ECHORADAR_ENABLE_ONNX=ON`: enable optional recognition/legacy direction
  package inference.
- `ECHORADAR_BUILD_TESTS=ON`: build the native regression suite.
- `ECHORADAR_BUILD_STEAM_AUDIO_RENDERER=ON`: build the optional offline tool.

```powershell
cmake -S . -B build-tests `
  -DECHORADAR_BUILD_APP=OFF `
  -DECHORADAR_BUILD_AUDIO_MONITOR=OFF `
  -DECHORADAR_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

## Source layout

```text
src/app          runtime orchestration and schema-3 session logging
src/audio        loopback capture, layouts, buffers, downmix, WAV
src/dsp          shared FFT/window helpers
src/radar        multichannel energy-vector radar
src/recognition  unchanged stereo recognition package/runtime
src/direction    retained direction-package compatibility
src/settings     schema-4 settings and per-display HUD state
src/ui           immutable snapshots and typed command queue
src/overlay      single dashboard and in-game HUD
src/support      JSON and hashing utilities
tests            native regression suite
ml               offline training and evaluation pipeline
```

Do not change serialized recognition/direction tensor names, shapes, package
versions, or preprocessing identifiers as part of radar work. Do not allocate,
lock, log, or perform inference from the audio callback. Discontinuities must
reset all timeline-dependent state together.

## Release checklist

Run the native and Python suites, then complete the real-device and visual
matrix in [evaluation.md](evaluation.md). Verify the frozen v1 branch
independently. A `v2.0.0-research` release must include `LICENSE`, `NOTICE`,
algorithm provenance, raw evaluation results, known limitations, and hashes for
published binaries.
