# EchoRadar

EchoRadar is a Windows C++20 project for recognizing important sounds in Counter-Strike 2 audio and, later, estimating their direction for a lightweight radar overlay.

The repository contains the original duration-based baseline and native v3 path, plus a CPU-ready v4 onset training pipeline. Existing model packages remain unchanged; no v4 model is promoted until locked real-data, accuracy, latency, source-suppression, export-parity, and native gates pass. See [the v4 training guide](ml/TRAINING_V4.md).

## Current status

| Area | Status | Evidence |
|---|---|---|
| Audio capture, buffers, STFT, and features | Working | Used by the existing diagnostic tools and automated tests. |
| Extracted-asset inventory | Working | 1,094 valid local WAVs mapped to gunshot, footstep, or named hard-negative subtypes. |
| Dataset splitting and mixture generation | Working | Leakage-safe family/surface splits and a pinned Steam Audio v4.8.1 offline spatial renderer. |
| V4 ML preparation/training/export | Ready to run | Disk-backed features, independent causal onset/source heads, CPU checkpoints, peak/scene/source calibration, and SHA-256 ONNX packaging. |
| Native recognition engine | Working infrastructure | Matching C++/Python `stereo-pcen-v2`, two-hop inference, independent 50 ms onset pulses, and legacy v1 loading. |
| Offline evaluator | Working | Reports event precision/recall/F1, false alerts per minute, latency, predictions, and error clips. |
| Timeline reviewer | Working prototype | Waveform/spectrogram review, model-seeded markers, hotkeys, uncertain state, and atomic JSONL saves. |
| Recognition accuracy | **Below gate** | Baseline results are recorded in [the recognition report](docs/recognition_baseline_v1.md). |
| Real CS2 validation | Waiting for recordings | Import/audit tooling is ready, but no held-out continuous gameplay sessions have been supplied or labeled yet. |
| Direction estimation | Deferred | Extracted assets have no known bearing labels; the current KNN code remains a placeholder. |
| Main application and overlay | Not integrated | Legacy application/native v3 paths remain unchanged until a v4 candidate meets its gate. |

The automated suites validate algorithms and contracts, not real-game accuracy. A passing test suite must not be interpreted as recognition performance.

## Recognition architecture

The native application still supports the existing v3 contract below. The
training-ready v4 contract uses a 240-sample hop, five stereo feature planes,
128 causal frames, independent gunshot/footstep onset heads, per-class
self/remote/unknown source heads, and local-peak post-processing. V4 native
integration intentionally waits for a trained candidate to pass its locked
Python evaluation.

```text
Native v3:
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

For existing native v1/v3 model inference, download and expand the CPU `Microsoft.ML.OnnxRuntime` NuGet package, then configure its extracted root:

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

For new CPU training, follow [the v4 guide](ml/TRAINING_V4.md) or use
`ml\run_cpu_training.ps1`.

Create a Python environment and install the locked training stack:

```powershell
py -3.12 -m venv ml\.venv
.\ml\.venv\Scripts\python.exe -m pip install -r ml\requirements-lock.txt
.\ml\.venv\Scripts\python.exe -m pip install -e ml
```

Create the onset-aligned grouped split and continuous sessions:

```powershell
echoradar-ml prepare-assets `
  --asset-root sounds `
  --output ml\generated\v4\asset-manifest.csv

echoradar-ml corpus `
  --manifest ml\generated\v4\asset-manifest.csv `
  --asset-root sounds `
  --output ml\generated\v4\sessions `
  --steam-audio-renderer build\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe
```

`corpus` creates 160 train, 40 development, and 40 locked-test sessions of 30 seconds, split equally between simple/complex with exactly 25% ambient-only sessions. It fails if the pinned renderer is missing or reports a version other than v4.8.1. The official SDK archive URL and SHA-256 are recorded in `external/steam-audio-v4.8.1.json`; extract that archive and build the offline-only renderer with `-DECHORADAR_BUILD_STEAM_AUDIO_RENDERER=ON -DSTEAMAUDIO_ROOT=<sdk-root>`.

Then train, verify export parity, and evaluate only the locked test sessions:

```powershell
echoradar-ml cache --sessions ml\generated\v4\sessions --output ml\generated\v4\feature-cache
echoradar-ml train --sessions ml\generated\v4\sessions --cache ml\generated\v4\feature-cache --output models\candidate
echoradar-ml parity --model models\candidate --fixture models\candidate\onnx_parity.npz
echoradar-ml evaluate `
  --sessions ml\generated\v4\sessions `
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

Reviewer keys: `1` gunshot, `2` footstep, `L` self, `R` remote, `N` unknown, `U` uncertain, `Delete` remove, `Space` play, `S` save, and arrow keys to move through time.

## Native tools

| Tool | Purpose |
|---|---|
| `sound_recognizer` | Existing v1/v3 live CPU monitor with probabilities, events, and inference timing. |
| `sound_eval` | Existing v1/v3 WAV evaluation through the exact native preprocessing and ONNX path. |
| `asset_inventory` | Validate and inventory the local extracted sound library. |
| `dataset_recorder` / `gunshot_visualizer` | Existing candidate capture and clip review workflow. |
| `audio_monitor`, `stft_monitor`, `feature_monitor` | Validate capture and DSP stages. |

Example:

```powershell
.\build\tools\sound_recognizer\Release\sound_recognizer.exe `
  --model models\legacy-v3 --device "CABLE"

.\build\tools\sound_eval\Release\sound_eval.exe `
  --model models\legacy-v3 `
  --wav ml\generated\sessions\test_simple_1.wav `
  --timeline ml\generated\sessions\test_simple_1.jsonl `
  --report ml\runs\candidate-native.json
```

## Accuracy gates and next work

The v4 recognition milestone remains open until a candidate meets every gate in
the generated `sound-eval-v4` report: 95% quiet remote-footstep and remote
gunshot recall, 95% recovery of gunshots at least 60 ms apart, under 1%
duplicate emissions, no more than 0.1 quiet false alerts/minute/class, no more
than two busy false alerts/minute total, p95 onset error at most 30 ms, p95
delivery latency at most 100 ms, at least 98% self suppression, and no more
than three recall points lost to suppression. Locked support and native
runtime gates must also pass before integration.

Next steps are deliberately data-driven:

1. Capture reviewed CS2 sessions with synchronized video and fixed audio settings until `echoradar-ml audit` passes the locked support gates.
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
