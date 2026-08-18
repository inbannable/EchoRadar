# EchoRadar

EchoRadar is a Windows C++20 application that captures system-output audio, recognizes gunshots and footsteps, estimates up to three concurrent 3D source directions, and displays them in a click-through HUD.

The supported runtime is deliberately narrow:

```text
Windows WASAPI loopback
  -> streaming stereo DSP
  -> stereo-onset-v4 recognition package
  -> multi-source direction package (optional)
  -> control UI, HUD, scene clips, and JSONL session log
```

Windows 10/11 x64, a C++20 toolchain, CMake 3.20+, and ONNX Runtime CPU are required for the application. The first CMake configure needs network access to fetch pinned source dependencies. Datasets, Python, and Steam Audio are training-only and are not runtime dependencies.

## Build

Expand the x64 CPU `Microsoft.ML.OnnxRuntime` NuGet package, then configure and build:

```powershell
cmake -S . -B build `
  -DECHORADAR_BUILD_APP=ON `
  -DECHORADAR_BUILD_AUDIO_MONITOR=ON `
  -DECHORADAR_ENABLE_ONNX=ON `
  -DONNXRUNTIME_ROOT="C:\path\to\expanded\Microsoft.ML.OnnxRuntime"
cmake --build build --config Release
```

Only `onnxruntime.dll` is copied beside `EchoRadar.exe`. Tests are opt-in with `-DECHORADAR_BUILD_TESTS=ON`; GoogleTest is fetched only for that configuration.

## Model packages

A recognition package contains:

```text
recognition-candidate/
  model.json       # package_version 4, stereo-onset-v4 metadata and SHA-256
  recognizer.onnx  # filename may vary; model.json names it
```

A direction package contains:

```text
direction-candidate/
  direction.json   # package_version 1, scene contract and SHA-256
  direction.onnx   # filename may vary; direction.json names it
```

Invalid, missing, or corrupt packages pause only their subsystem. No legacy recognizer or direction fallback is selected.

## Run

Recognition only:

```powershell
.\build\src\app\Release\EchoRadar.exe --model models\recognition-candidate
```

Recognition plus direction:

```powershell
.\build\src\app\Release\EchoRadar.exe `
  --model models\recognition-candidate `
  --direction-model models\direction-candidate
```

Use `--list-audio-outputs` to list render endpoints, `--audio-output-id <id>` to pin one, `--settings <json>` to override the settings file, and `--no-overlay` for headless operation. The loopback-only `audio_monitor` can inspect levels or record the exact runtime PCM.

By default, settings are stored at `%LOCALAPPDATA%\EchoRadar\settings.json`. Scene clips and the schema-2 JSONL log are written beneath `%LOCALAPPDATA%\EchoRadar\sessions\`. An existing `direction-calibration.tsv` is never modified or deleted during schema-2 to schema-3 settings migration.

## Current milestone

The runtime and model contracts are implemented. The active milestone is direction candidate training, packaged evaluation, and held-out real-CS2 validation. See [runtime](docs/runtime.md), [development](docs/development.md), [direction validation](docs/direction-validation.md), and the [ML guide](ml/README.md).

Local `sounds/`, `models/`, `recordings/`, generated corpora, runs, caches, and checkpoints are ignored and must not be redistributed.
