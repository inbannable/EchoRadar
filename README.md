# EchoRadar

EchoRadar is a Windows C++20 project for recognizing important sounds in Counter-Strike 2 audio, estimating their direction per event, and displaying uncertainty arcs in a lightweight center-screen HUD. It captures the Windows system-output stream directly, so playback can stay on the headphones without OBS or VB-CABLE in the capture path.

The repository contains the original duration-based baseline and native v3 path, plus the CPU-ready v4 onset pipeline and an experimental native v4 runtime. A newly trained candidate can be connected to the main application immediately for engineering validation; it is not promoted as a production model until the locked real-data, accuracy, latency, source-suppression, export-parity, and native gates pass. See [the v4 training guide](ml/TRAINING_V4.md) and [the runtime guide](docs/audio_capture_v4_runtime.md).

## Current status

| Area | Status | Evidence |
|---|---|---|
| Built-in system-output capture | Implemented; Windows smoke test required | WASAPI loopback follows the default render endpoint, supports a pinned endpoint, recovers after endpoint loss, and does not reroute playback. |
| Audio buffers, STFT, and features | Working | Lock-free callback handoff, bounded backlog, discontinuity generations, and Python/C++ v4 feature parity are covered by automated tests. |
| Extracted-asset inventory | Working | 1,094 valid local WAVs mapped to gunshot, footstep, or named hard-negative subtypes. |
| Dataset splitting and mixture generation | Working | Leakage-safe family/surface splits and a pinned Steam Audio v4.8.1 offline spatial renderer. |
| V4 ML preparation/training/export | Ready to run | Disk-backed features, independent causal onset/source heads, CPU checkpoints, peak/scene/source calibration, and SHA-256 ONNX packaging. |
| Native recognition engine | Experimental v4 integrated | Matching `stereo-onset-v4` five-plane streaming features, startup padding, two-hop ONNX inference, scene thresholds, peak spacing, source hints, and self suppression. Legacy v1/v3 tools remain available. |
| Offline evaluator | Working | Reports event precision/recall/F1, false alerts per minute, latency, predictions, and error clips. |
| Timeline reviewer | Working prototype | Waveform/spectrogram review, model-seeded markers, hotkeys, uncertain state, and atomic JSONL saves. |
| Recognition accuracy | **Below gate** | Baseline results are recorded in [the recognition report](docs/recognition_baseline_v1.md). |
| Real CS2 validation | Waiting for recordings | Import/audit tooling is ready, but no held-out continuous gameplay sessions have been supplied or labeled yet. |
| Direction estimation | Working synthetic baseline + guided calibration | Each enabled event receives one 24-bin stereo estimate with confidence, uncertainty, optional secondary mode, and a profile-conditioned real-CS2 calibration path. |
| Direction HUD | Implemented; Windows/CS2 smoke test required | A separate topmost click-through overlay follows CS2 in borderless mode and draws class-colored uncertainty arcs at screen center. |
| Main application and event chart | Experimental v4 connected | `EchoRadar.exe` provides Live, Recognition, Direction, Calibration, Overlay, and Audio/System pages while preserving the full sound-tuning table and Pulse width control. |

The automated suites validate algorithms and contracts, not real-game accuracy. A passing test suite must not be interpreted as recognition performance.

## Runtime architecture

The main executable uses one bounded PCM stream shared by capture and recognition. The audio callback only converts/copies into the SPSC ring and updates levels; feature extraction and ONNX inference run on the processing thread. A default-device change, endpoint restart, ring overflow, or excessive consumer backlog creates a new stream generation and resets the causal recognizer rather than joining unrelated audio across a gap.

```text
Windows default render endpoint (headphones/speakers)
    -> WASAPI loopback (48 kHz stereo float32; no playback reroute)
    -> lock-free SPSC PCM ring + bounded backlog/discontinuity generation
    -> 1024 FFT / 240 hop / 64 mel bins
    -> PCEN, absolute dB, ILD, coherence real, coherence imaginary
    -> left-padded rolling 128-frame context; ONNX every two feature frames
    -> independent gunshot/footstep onset and self/remote/unknown heads
    -> quiet/busy thresholds + local peaks + per-class spacing
    -> self events suppressed; remote/unknown events delivered to the app
```

`other` audio is used as background and hard-negative training material. It is never emitted as a user-facing event. The model outputs are independent, so simultaneous categories are allowed.

V4 events retain calibrated acoustic `onsetSample`, peak-confirmation `detectedSample`, actual real-time `deliveredSample`, source/scene metadata, and the stream generation. `logmel-v1` and `stereo-pcen-v2` packages retain their original behavior in the legacy diagnostic tools.

## Build and test

The default build does not require ONNX Runtime:

```powershell
cmake -S . -B build -DECHORADAR_BUILD_TESTS=ON -DECHORADAR_BUILD_TOOLS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For native model inference (including the main application's v4 path), download and expand the CPU `Microsoft.ML.OnnxRuntime` NuGet package, then configure its extracted root:

```powershell
cmake -S . -B build `
  -DECHORADAR_BUILD_TESTS=ON `
  -DECHORADAR_BUILD_TOOLS=ON `
  -DECHORADAR_ENABLE_ONNX=ON `
  -DONNXRUNTIME_ROOT="C:\path\to\expanded\Microsoft.ML.OnnxRuntime"
cmake --build build --config Release
```

The build copies `onnxruntime.dll` beside `EchoRadar.exe`, `sound_eval.exe`, and `sound_recognizer.exe`. Model packages are rejected if their checksum, tensor contract, class/source order, calibrated policy, or preprocessing contract is incompatible.

## Run built-in capture and v4

Place the training output at `models\v4-candidate` or pass another package directory explicitly. EchoRadar follows the current Windows default output endpoint by default:

```powershell
.\build\src\app\Release\EchoRadar.exe --model models\v4-candidate
```

On Windows, the executable opens the V4 event chart and direction HUD by default. The timeline shows
gunshots and footsteps against the current stream time; the V4 tune table applies
threshold, spacing, onset-offset, scene, self-suppression, and pulse-width changes
on the next audio block. The live diagnostics panel also shows the current
scene-activity sound-level score, capture RMS/peak in dBFS, and raw gunshot/footstep
onset scores before peak/event gating. Use `--no-overlay` only for headless
capture/recognition. Direction defaults to both footsteps and gunshots; either
class can be disabled separately. The HUD defaults to CS2-foreground-only and expects
Fullscreen Windowed/Borderless mode. Press `Ctrl+Alt+O` to hide or restore it.

Direction and overlay settings are persisted in
`%LOCALAPPDATA%\EchoRadar\settings.json`. Guided calibration and the rationale
for audio-profile conditioning are documented in
[the direction guide](docs/direction_estimation.md).

List render endpoints and optionally pin one by its opaque ID:

```powershell
.\build\src\app\Release\EchoRadar.exe --list-audio-outputs
.\build\src\app\Release\EchoRadar.exe `
  --audio-output-id "ma:0123456789abcdef" `
  --model models\v4-candidate
```

The default route automatically moves to a newly selected Windows default output. A pinned route waits and retries if that endpoint disappears. If the model package is missing, corrupt, incompatible, or fails its startup inference check, capture remains active and recognition is paused; the legacy recognizer is not used as an implicit fallback.

Use `audio_monitor` to verify levels or save the exact loopback PCM seen by the runtime:

```powershell
.\build\tools\audio_monitor\Release\audio_monitor.exe --seconds 10
.\build\tools\audio_monitor\Release\audio_monitor.exe `
  --record recordings\loopback-smoke.wav --seconds 30
```

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
| `EchoRadar` | Built-in default-output loopback plus experimental v4 live inference. |
| `audio_monitor` | List output endpoints, inspect loopback state/levels/backlog, and record runtime-equivalent PCM. |
| `sound_recognizer` | Existing v1/v3 live CPU monitor with probabilities, events, and inference timing. |
| `sound_eval` | Existing v1/v3 WAV evaluation through the exact native preprocessing and ONNX path. |
| `asset_inventory` | Validate and inventory the local extracted sound library. |
| `dataset_recorder` / `gunshot_visualizer` | Existing candidate capture and clip review workflow. |
| `stft_monitor`, `feature_monitor` | Validate legacy input-device DSP stages. |

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

The v4 production-recognition milestone remains open until a candidate meets every gate in
the generated `sound-eval-v4` report: 95% quiet remote-footstep and remote
gunshot recall, 95% recovery of gunshots at least 60 ms apart, under 1%
duplicate emissions, no more than 0.1 quiet false alerts/minute/class, no more
than two busy false alerts/minute total, p95 onset error at most 30 ms, p95
delivery latency at most 100 ms, at least 98% self suppression, and no more
than three recall points lost to suppression. Locked support and native
runtime gates must also pass before the candidate is promoted beyond the experimental integration.

Next steps are deliberately data-driven:

1. Capture reviewed CS2 sessions with synchronized video and fixed audio settings until `echoradar-ml audit` passes the locked support gates.
2. Use the timeline reviewer to correct model suggestions and mark ambiguous sounds uncertain.
3. Mine the generated error clips, improve the model/taxonomy, and rerun the locked reports without moving test sessions into training.
4. Run the candidate through the native application, compare Python/native traces and timing, and keep it marked experimental until the gates pass.
5. Generate the known-bearing direction corpus, then record calibrated and held-out real-CS2 bearings for each supported audio profile.

The direction milestone now includes a working synthetic baseline and the
known-bearing corpus command. Its next accuracy step is collecting repeatable
real-CS2 calibration and held-out bearing sessions for each supported audio
profile, not changing the event recognizer.

Local extracted sounds, generated sessions, gameplay recordings, and model packages are ignored by Git and must not be redistributed.

## Repository layout

```text
src/audio/             Built-in WASAPI loopback, device routing/recovery, PCM ring, and stream contract
src/recognition/       Native v1/v3/v4 preprocessing, package validation, ONNX inference, events, and metrics
ml/                    Python split, mixture, training, inference, evaluation, and annotation package
tools/sound_eval/      Native offline evaluator
tools/sound_recognizer Native live monitor
tools/steam_audio_renderer Pinned v4.8.1 offline dataset renderer (optional build)
src/app/               Built-in capture and experimental v4 application wiring
src/localization/      Per-event stereo features, circular estimates, and guided calibration
src/settings/          Persisted audio-profile, localization, HUD, and logging settings
src/overlay/           Interactive control window and click-through direction HUD
src/dsp,...            Shared DSP, detector, event, feature, and tracking modules
tests/                 Native GoogleTest suite
docs/                  Milestone contracts and measured reports
```

Further implementation detail is in [the recognition milestone document](docs/recognition_milestone.md).
