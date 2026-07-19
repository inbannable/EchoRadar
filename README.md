# EchoRadar

EchoRadar is a Windows C++20 project for detecting important sounds in FPS game audio and, eventually, showing their estimated direction in a lightweight radar overlay.

The project already has a working audio-analysis and dataset-tooling foundation. It is **not yet a finished end-user application**: the real-time gunshot pipeline currently lives in the diagnostic tools, while footstep detection, direction estimation, the production overlay, and final application integration still need to be completed.

## Current status

| Area | Status | What that means |
|---|---|---|
| Audio capture and device selection | Working | Captures stereo float audio at 48 kHz through miniaudio. |
| Ring buffers and audio history | Working | Supports real-time processing and event-centered clip extraction. |
| STFT and feature extraction | Working | Produces stereo spectra and transient/spectral features. |
| Multi-stage gunshot detector | Working prototype | Implemented and unit-tested with synthetic feature sequences; real game-audio accuracy is not yet measured. |
| Gunshot monitor and visualizer | Working tools | Show detector state, scores, features, and candidate decisions. |
| Dataset recorder and Dataset Studio | Safer working prototype | Uses restart-safe IDs, atomic event publishing, complete manifests, review status, replay, filtering, and keyboard-assisted labeling. |
| Extracted-asset inventory | Working | Scans local WAV assets without modifying them, applies four-class rules, records audio metadata and SHA-256 hashes, and reports duplicates and low-confidence labels. |
| Footstep detector | Placeholder | The current implementation does not emit events. |
| Direction estimator | Partial prototype | Basic nearest-neighbor inference exists, but calibration, persistence, confidence, and circular averaging are unfinished. |
| Direction tracker | Basic implementation | Circular smoothing exists but is not yet validated as part of a complete localization pipeline. |
| Production overlay | Placeholder | The Windows/DX11 overlay shell exists, but rendering is not implemented. |
| Main `EchoRadar` application | Not integrated | It still uses legacy placeholder detectors instead of the working gunshot pipeline. |

The current source builds successfully, and the automated suite contains **83 passing tests**. These tests cover core algorithms, dataset operations, and extracted-asset inventory, but they do not yet prove accuracy on recorded game audio or validate the complete application end to end.

## Working pipeline

The most complete pipeline is used by the gunshot tools:

```text
AudioCapture
    -> AudioRingBuffer
    -> STFTProcessor
    -> FeatureExtractor
    -> GunshotEventDetector
    -> monitor / visualizer / dataset recorder
```

The intended final pipeline is:

```text
Game audio
    -> capture and buffering
    -> spectral features
    -> gunshot and footstep events
    -> direction estimation and tracking
    -> transparent radar overlay
```

## Repository layout

```text
src/
  audio/          Audio capture, devices, and PCM history
  dsp/            Ring buffer, windows, and streaming STFT
  features/       Feature extraction and feature history
  detector/       Multi-stage gunshot event detector
  events/         Legacy gunshot and placeholder footstep detectors
  dataset/        Dataset index, labels, metadata, and quality checks
  localization/   Partial KNN direction estimator
  tracking/       Direction smoothing
  overlay/        Production overlay placeholder
  app/            Main application shell

tools/
  audio_monitor/       Audio-device and level validation
  stft_monitor/        Live spectrum validation
  feature_monitor/     Live feature validation
  gunshot_monitor/     Text-mode detector diagnostics
  gunshot_visualizer/  Detector dashboard and Dataset Studio
  dataset_recorder/    Event-centered audio and feature capture

tests/             GoogleTest suites
docs/              Detailed milestone notes
dataset/           Local recorded data; ignored by Git except `.gitkeep`
```

## Requirements

- Windows 10 or 11
- Visual Studio 2022 or newer with **Desktop development with C++**
- Windows SDK 10.0 or newer
- CMake 3.20 or newer
- Git

CMake fetches the pinned versions of KissFFT, miniaudio, Dear ImGui, and GoogleTest during configuration.

## Build and test

Run these commands from the repository root. They deliberately use relative paths, so the project can be moved without editing project files.

```powershell
cmake -S . -B build -DECHORADAR_BUILD_TESTS=ON -DECHORADAR_BUILD_TOOLS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### After moving the repository

A CMake build directory stores absolute paths. If the repository is moved, the old build cache must be regenerated before building again:

```powershell
cmake --fresh -S . -B build -DECHORADAR_BUILD_TESTS=ON -DECHORADAR_BUILD_TOOLS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

If the installed CMake version does not support `--fresh`, remove only the generated `build` directory, then run the normal build commands above. Never copy a `build` directory between repository locations.

## Available tools

After a Release build with the Visual Studio generator:

| Tool | Executable | Purpose |
|---|---|---|
| Audio monitor | `build\tools\audio_monitor\Release\audio_monitor.exe` | List devices and verify live stereo levels. |
| STFT monitor | `build\tools\stft_monitor\Release\stft_monitor.exe` | Verify streaming spectral output. |
| Feature monitor | `build\tools\feature_monitor\Release\feature_monitor.exe` | Inspect extracted audio features. |
| Gunshot monitor | `build\tools\gunshot_monitor\Release\gunshot_monitor.exe` | Inspect detector state, confidence, and emitted events. |
| Gunshot visualizer | `build\tools\gunshot_visualizer\Release\gunshot_visualizer.exe` | Tune and explain the detector; browse and label datasets. |
| Dataset recorder | `build\tools\dataset_recorder\Release\dataset_recorder.exe` | Capture event-centered WAV, CSV, and JSON records. |
| Asset inventory | `build\tools\asset_inventory\Release\asset_inventory.exe` | Classify and validate a local extracted-WAV library without changing its source files. |

All capture tools support the default input device and a partial device-name match. For example:

```powershell
.\build\tools\audio_monitor\Release\audio_monitor.exe --list-devices-detailed
.\build\tools\audio_monitor\Release\audio_monitor.exe --device "Stereo Mix"
.\build\tools\gunshot_visualizer\Release\gunshot_visualizer.exe --device "CABLE" --dataset-root "dataset"
```

For game audio, use a Windows loopback input such as Stereo Mix or route game audio through a virtual cable. A physical microphone captures room audio and is usually not suitable for detector evaluation.

## Dataset format

The recorder creates six label folders under `dataset/`:

```text
gunshot/
footstep/
reload/
switch/
ambient/
unknown/
```

Each event folder contains:

```text
audio.wav       400 ms stereo PCM16 clip
features.csv    Per-frame extracted features and detector values
metadata.json   Capture and detector metadata
```

Freshly recorded events initially go to `unknown/` and can be labeled in Dataset Studio. Metadata also records the recorder session, the detector's final decision, and whether a person has reviewed the clip.

> **Important:** the recorder is now safe enough for a small pilot dataset, but it still captures event-centered detector candidates rather than complete continuous sessions. A large evaluation dataset should wait for Phase 2 because candidate-only recording cannot reveal gunshots that the detector missed entirely.

## Extracted asset workflow

Locally extracted game assets are the primary source corpus for offline development. Keep them outside version control: `pilot-datasets/**` is ignored, and extracted audio must not be committed or redistributed with EchoRadar.

Generate a deterministic local inventory with:

```powershell
.\build\tools\asset_inventory\Release\asset_inventory.exe `
  --asset-root "Z:\CODE\EchoRadar\pilot-datasets\sounds" `
  --output-dir "Z:\CODE\EchoRadar\pilot-datasets\asset-inventory"
```

The tool writes:

```text
asset_manifest.csv   Every valid WAV, label, subtype, source group, format, levels, hash, and duplicate relationship
review_needed.csv    Only invalid or low-confidence classifications
summary.json         Counts and total duration
```

The four detector-oriented labels are `gunshot`, `footstep`, `mechanical`, and `other`. Reloads, weapon switches, magazine actions, bolts, slides, pumps, draws, and related handling sounds belong to `mechanical`. The inventory reads PCM8/PCM16 WAV headers and levels but does not yet resample or copy audio.

See [Milestone 7: Extracted Asset Inventory](docs/milestone7_asset_inventory.md) for the classification contract and completion gate.

## Development plan

Work should proceed in this order. Each phase has an explicit completion gate so a milestone is not marked complete merely because its classes exist.

### Phase 1 — Make dataset recording safe

**Status: completed.**

1. Replace session-local event numbering with collision-proof IDs.
2. Refuse to overwrite an existing event.
3. Write an event into a temporary directory and publish it atomically only after all three files succeed.
4. Make delete, trash, restore, and undo conflict-safe.
5. Export all labels through one canonical manifest implementation.
6. Store one clip per physical candidate together with its full detector-decision history.
7. Add tests for recorder restarts, ID collisions, partial writes, trash conflicts, and manifests.

**Complete when:** repeated and interrupted recorder sessions cannot overwrite, silently discard, or publish partial data.

### Phase 2 — Add offline replay and measurable evaluation

**Status: extracted-asset inventory complete; conversion, generation, and replay pending.**

1. Inventory local extracted WAV assets with hashes, rule provenance, duplicate relationships, and a low-confidence review list.
2. Decode PCM8/PCM16 mono/stereo assets and resample them to the production 48 kHz stereo format.
3. Split by source asset, weapon family, footstep surface, and duplicate hash before augmentation.
4. Generate deterministic continuous synthetic sessions with exact ground-truth event timelines.
5. Add a WAV replay path that uses the same STFT, feature, and detector code as live capture.
6. Report precision, recall, F1, false positives per minute, detection latency, and burst shot-count accuracy.
7. Keep only redistributable synthetic fixtures in automated tests; never commit extracted game audio.

**Complete when:** detector changes can be compared from one reproducible report without launching a game or using a live audio device.

### Phase 3 — Validate the gunshot detector with real data

1. Use extracted assets and generated mixtures for development, then validate on small held-out continuous gameplay sessions.
2. Cover multiple weapons, firing modes, distances, maps, volume levels, and audio-routing setups.
3. Include difficult negatives: footsteps, mechanical handling, UI sounds, ambience, music, and speech.
4. Add session, source, weapon, device, routing, and gain metadata.
5. Establish a baseline before tuning thresholds or feature weights.

**Complete when:** accuracy targets are written down and met on held-out recording sessions.

### Phase 4 — Integrate the working gunshot pipeline

1. Replace the legacy gunshot detector in the main application with `GunshotEventDetector`.
2. Consolidate the legacy and streaming audio paths.
3. Move DSP-to-UI communication onto a thread-safe event queue.
4. Add a headless WAV-to-application-event integration test.
5. Verify startup, shutdown, audio-device failure, and long-running capture behavior.

**Complete when:** a known gunshot recording reliably reaches the main application event layer without any placeholder detector.

### Phase 5 — Implement and validate footsteps

1. Define the event shape, cadence rules, confidence model, and acceptance metrics.
2. Use labeled footstep and hard-negative recordings to drive implementation.
3. Test confusion with gunshots, reloads, switching, ambience, and music.
4. Integrate footsteps only after offline targets are met.

**Complete when:** the detector meets its documented targets on held-out sessions and is connected to the application event layer.

### Phase 6 — Complete localization and tracking

1. Define a calibration workflow for known angles and each supported game/HRTF/device setup.
2. Finish normalized ILD/ITD and spectral direction features.
3. Implement circular, distance-weighted KNN, model persistence, versioning, and out-of-distribution rejection.
4. Make tracking use measurement confidence.
5. Report angular error, percentile error, and coverage.

**Complete when:** a versioned model can be trained, saved, loaded, rejected when incompatible, and evaluated reproducibly.

### Phase 7 — Implement the production overlay

1. Create the transparent, click-through Win32/DX11 window.
2. Render confidence-aware gunshot and footstep markers with safe lifetimes.
3. Handle device loss, display scaling, settings, and clean shutdown.
4. Verify that the UI and DSP threads do not share mutable state unsafely.

**Complete when:** the overlay runs reliably over a game and survives display/device changes without disrupting audio processing.

### Phase 8 — System validation and release readiness

1. Measure end-to-end latency, CPU use, memory, dropped audio, and writer backlog.
2. Run long capture and overlay soak tests.
3. Add continuous Windows builds and automated tests.
4. Add CMake presets, installation/package instructions, and a formal license file.
5. Update all documentation from measured behavior and remove obsolete compatibility paths.

**Complete when:** the complete application meets its published accuracy, latency, stability, and packaging requirements.

## Milestone notes

- [Milestone 5: multi-stage gunshot detector](docs/milestone5_gunshot_detector.md)
- [Milestone 6: dataset pipeline](docs/milestone6_dataset_pipeline.md)
- [Developer setup guide](DEVELOPER_SETUP.md)

These notes describe implemented work. This README is the authoritative high-level status and future plan until more detailed specifications are added for later phases.
