# EchoRadar

EchoRadar is a Windows C++20 project for recognizing important sounds in Counter-Strike 2 audio and, later, estimating their direction for a lightweight radar overlay.

The repository contains the original duration-based baseline plus the v3 onset-first training and runtime path. The two existing model packages remain unchanged; no v3 model is promoted until the locked synthetic, gain, confusion, onset, and native gates pass.

## Current status

| Area | Status | Evidence |
|---|---|---|
| Audio capture, buffers, STFT, and features | Working | Used by the existing diagnostic tools and automated tests. |
| Extracted-asset inventory | Working | 1,094 valid local WAVs classified as gunshot, footstep, mechanical, or other. |
| Dataset splitting and mixture generation | Working | Leakage-safe family/surface splits and a pinned Steam Audio v4.8.1 offline spatial renderer. |
| ML training and ONNX export | Working | Two-head causal CRNN, focal/margin losses, hard-negative retraining, onset calibration, and SHA-256 packaging. |
| Native recognition engine | Working infrastructure | Matching C++/Python `stereo-pcen-v2`, two-hop inference, independent 50 ms onset pulses, and legacy v1 loading. |
| Offline evaluator | Working | Reports event precision/recall/F1, false alerts per minute, latency, predictions, and error clips. |
| Timeline reviewer | Working prototype | Waveform/spectrogram review, model-seeded markers, hotkeys, uncertain state, and atomic JSONL saves. |
| Recognition accuracy | **Below gate** | Baseline results are recorded in [the recognition report](docs/recognition_baseline_v1.md). |
| Real CS2 validation | Waiting for recordings | No held-out continuous gameplay sessions have been supplied or labeled yet. |
| Direction estimation | Deferred | Extracted assets have no known bearing labels; the current KNN code remains a placeholder. |
| Main application and overlay | Not integrated | Legacy application path remains unchanged until recognition meets its gate. |

The native suite contains **93 passing tests** and the ML package contains **13 passing tests**. These validate algorithms and contracts, not real-game accuracy.

## Recognition architecture

```text
48 kHz stereo PCM
    -> independent L/R spectra averaged in the power domain
    -> 1024 FFT / 512 hop / 64-bin causal PCEN + absolute dB
    -> rolling 96-frame causal context
    -> objectness head x three independent class heads
    -> inference every two hops; per-class rearm and refractory timing
    -> independent exact 50 ms gunshot / footstep / mechanical onset pulses
```

`other` audio is used as background and hard-negative training material. It is never emitted as a user-facing event. The model outputs are independent, so simultaneous categories are allowed.

V3 emits on the first threshold crossing and records both calibrated acoustic `onsetSample` and delivery-time `detectedSample`. Separate class state allows true overlaps. `logmel-v1` packages retain their original mono preprocessing and segment semantics for baseline comparison.

## Build and test

The default build does not require ONNX Runtime:

```powershell
cmake -S . -B build -DECHORADAR_BUILD_TESTS=ON -DECHORADAR_BUILD_TOOLS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For native model inference, download and expand the CPU `Microsoft.ML.OnnxRuntime` NuGet package, then configure its extracted root:

```powershell
cmake -S . -B build `
  -DECHORADAR_BUILD_TESTS=ON `
  -DECHORADAR_BUILD_TOOLS=ON `
  -DECHORADAR_ENABLE_ONNX=ON `
  -DONNXRUNTIME_ROOT="C:\path\to\expanded\Microsoft.ML.OnnxRuntime"
cmake --build build --config Release
```

The build copies `onnxruntime.dll` beside `sound_eval.exe` and `sound_recognizer.exe`. Model packages are rejected if their checksum, class order, or preprocessing contract is incompatible.

## ML workflow

Create a Python environment and install the locked training stack:

```powershell
py -3.12 -m venv ml\.venv
.\ml\.venv\Scripts\python.exe -m pip install -r ml\requirements-lock.txt
.\ml\.venv\Scripts\python.exe -m pip install -e ml
```

Create the grouped split and continuous sessions:

```powershell
echoradar-ml split `
  --inventory pilot-datasets\asset-inventory\asset_manifest.csv `
  --output ml\generated\split_manifest.csv

echoradar-ml corpus `
  --manifest ml\generated\split_manifest.csv `
  --asset-root pilot-datasets\sounds `
  --output ml\generated\sessions `
  --steam-audio-renderer build\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe
```

`corpus` creates 160 train, 40 development, and 40 locked-test sessions of 30 seconds, split equally between simple/complex with exactly 25% ambient-only sessions. It fails if the pinned renderer is missing or reports a version other than v4.8.1. The official SDK archive URL and SHA-256 are recorded in `external/steam-audio-v4.8.1.json`; extract that archive and build the offline-only renderer with `-DECHORADAR_BUILD_STEAM_AUDIO_RENDERER=ON -DSTEAMAUDIO_ROOT=<sdk-root>`.

Then train, verify export parity, and evaluate only the locked test sessions:

```powershell
echoradar-ml train --sessions ml\generated\sessions --output models\candidate
echoradar-ml parity --model models\candidate --fixture models\candidate\onnx_parity.npz
echoradar-ml evaluate `
  --sessions ml\generated\sessions `
  --model models\candidate `
  --output ml\runs\candidate-evaluation
```

Review a continuous recording, optionally seeded with evaluator predictions:

```powershell
echoradar-ml annotate `
  --wav recordings\session.wav `
  --labels recordings\session.labels.jsonl `
  --predictions recordings\session.predictions.jsonl
```

Reviewer keys: `1` gunshot, `2` footstep, `3` mechanical, `U` uncertain, `Delete` remove, `Space` play, `S` save, and arrow keys to move through time.

## Native tools

| Tool | Purpose |
|---|---|
| `sound_recognizer` | Live CPU recognition monitor with probabilities, emitted events, and inference timing. |
| `sound_eval` | WAV + JSONL event evaluation through the exact native preprocessing and ONNX path. |
| `asset_inventory` | Validate and inventory the local extracted sound library. |
| `dataset_recorder` / `gunshot_visualizer` | Existing candidate capture and clip review workflow. |
| `audio_monitor`, `stft_monitor`, `feature_monitor` | Validate capture and DSP stages. |

Example:

```powershell
.\build\tools\sound_recognizer\Release\sound_recognizer.exe `
  --model models\candidate --device "CABLE"

.\build\tools\sound_eval\Release\sound_eval.exe `
  --model models\candidate `
  --wav ml\generated\sessions\test_simple_1.wav `
  --timeline ml\generated\sessions\test_simple_1.jsonl `
  --report ml\runs\candidate-native.json
```

## Accuracy gates and next work

The recognition milestone remains open until a candidate achieves:

- Less than 1% wrong-class co-triggers and both classes recovered in at least 90% of true gunshot/footstep overlaps.
- At least 90% remote footstep recall overall and 85% in every azimuth quadrant, with no more than five recall points lost at -12 or -24 dB.
- No more than 0.1 false alerts/minute/class on ambient and two total false alerts/minute in complex scenes, while retaining the existing simple/complex class-recall gates.
- Median absolute onset error at most 30 ms, p95 at most 60 ms, gunshots separated at 60 ms, and footsteps at 100 ms.
- Native ONNX parity below `1e-4`, inference p95 below 2 ms, and no dropped audio at the two-hop rate.

Next steps are deliberately data-driven:

1. Capture at least three continuous CS2 sessions with synchronized replay/video context and fixed audio settings.
2. Use the timeline reviewer to correct model suggestions and mark ambiguous sounds uncertain.
3. Mine the generated error clips, improve the model/taxonomy, and rerun the locked reports without moving test sessions into training.
4. Integrate the shared recognizer into the main application only after the gates pass.
5. Start localization separately with known-bearing rendered audio and the same recorded CS2 audio profile.

Local extracted sounds, generated sessions, gameplay recordings, and model packages are ignored by Git and must not be redistributed.

## Repository layout

```text
src/recognition/       Native preprocessing, package validation, ONNX inference, events, and metrics
ml/                    Python split, mixture, training, inference, evaluation, and annotation package
tools/sound_eval/      Native offline evaluator
tools/sound_recognizer Native live monitor
tools/steam_audio_renderer Pinned v4.8.1 offline dataset renderer (optional build)
src/audio,dsp,...      Existing capture, DSP, prototype detector, localization, and app modules
tests/                 Native GoogleTest suite
docs/                  Milestone contracts and measured reports
```

Further implementation detail is in [the recognition milestone document](docs/recognition_milestone.md).
