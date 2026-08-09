# Built-in audio capture and experimental v4 runtime

## Scope

EchoRadar captures the mix sent to a Windows render endpoint with WASAPI loopback. It does not change the Windows playback route, install a virtual device, or require OBS/VB-CABLE. The default configuration follows the Windows default output device; a command-line endpoint ID can pin capture to one device.

The runtime integration is deliberately limited to audio capture and experimental v4 sound events. The Windows app now renders those events in a rolling time chart and exposes the V4 event policy as a live tune table; localization is unchanged. Passing native tests does not establish real-game recognition accuracy.

## Data flow

```text
WASAPI render endpoint
  -> miniaudio callback: float32 stereo conversion, ring write, channel levels
  -> SPSC ring: one-second capacity, newest-input drop on overflow
  -> processing thread: 10 ms pulls, backlog bound, stream reset handling
  -> stereo-onset-v4 features: [5, 128, 64]
  -> ONNX onset/source heads
  -> calibrated scene thresholds, local peaks, spacing, self suppression
  -> V4SoundEvent callback
```

The callback never performs FFT, ONNX inference, logging, endpoint enumeration, or blocking synchronization. The processing thread owns capture polling and all recognizer state.

## Endpoint policy and recovery

- `FollowDefault` is the normal route. EchoRadar opens the default render endpoint and checks its identity once per second. A default-device change starts a fresh capture generation.
- `Fixed` opens the endpoint selected by `--audio-output-id`. It never silently falls back to another output.
- A missing or stopped endpoint enters `Recovering`. Retries use exponential backoff from 250 ms to 5 seconds.
- Capture output is always 48 kHz, stereo, interleaved float32. miniaudio/WASAPI performs native format conversion when required.
- Ring overflow discards the incomplete stream and starts a new generation. If the consumer falls more than 200 ms behind, it discards old PCM, retains the newest 20 ms, and also starts a new generation. The recognizer resets its STFT, PCEN, causal context, peak trace, and pending events at every generation boundary.

The stable consumer boundary is `AudioBlockView` / `IRealtimeAudioConsumer`. Borrowed PCM is valid only during `OnAudio`; `OnStreamReset` is sent before audio from a new generation is consumed.

## Model package contract

The main app accepts only a package with `package_version: 4`, `preprocessing_version: stereo-onset-v4`, and the exported v4 tensor/policy contract:

- input `features`: float32 `[1, 5, 128, 64]`;
- output `onset_probabilities`: float32 `[1, 128, 2]`;
- output `source_probabilities`: float32 `[1, 128, 2, 3]`;
- class order `gunshot,footstep`;
- source order `self,remote,unknown`;
- 48 kHz, FFT 1024, hop 240, 64 mel bins, inference stride 2;
- calibrated quiet/busy thresholds, peak lookahead, minimum spacing, onset offsets, scene cutoff, and self-suppression threshold;
- an ONNX SHA-256 matching `model.json`.

At startup EchoRadar performs one silent padded inference to validate input/output names and shapes and warm up ONNX Runtime. Missing or invalid models pause recognition without stopping capture. A later inference failure also pauses recognition for the current run; there is no implicit legacy-model fallback.

## Commands

```powershell
# List output endpoints and the stable opaque IDs used by this build/backend.
.\build\src\app\Release\EchoRadar.exe --list-audio-outputs

# Follow the Windows default output and load the default model directory.
.\build\src\app\Release\EchoRadar.exe

# Use an explicit candidate package and disable the event chart UI for headless runs.
.\build\src\app\Release\EchoRadar.exe `
  --model models\v4-candidate `
  --no-overlay

# Pin capture to one output endpoint.
.\build\src\app\Release\EchoRadar.exe `
  --audio-output-id "ma:0123456789abcdef" `
  --model models\v4-candidate

# Capture-only smoke test and exact PCM recording.
.\build\tools\audio_monitor\Release\audio_monitor.exe --seconds 10
.\build\tools\audio_monitor\Release\audio_monitor.exe `
  --record recordings\loopback-smoke.wav --seconds 30
```

The old input-device route remains available only for diagnostics:

```powershell
.\build\tools\audio_monitor\Release\audio_monitor.exe `
  --source input --device "CABLE" --seconds 10
```

## Windows validation checklist

Run this checklist on the target laptop after the training process is finished or when spare resources are available:

1. Start `audio_monitor --seconds 10`; play known stereo audio through the headphones and confirm non-zero L/R levels without OBS or VB-CABLE.
2. Record 30 seconds and verify the WAV is 48 kHz stereo, contains the expected system mix, and does not contain microphone-only audio.
3. While following the default endpoint, switch Windows playback from headphones to speakers and back. Confirm the endpoint name and stream generation change, then confirm levels resume.
4. Pin an endpoint, disconnect it, and reconnect it. Confirm `Recovering` changes back to `Running` without capturing another output.
5. Run `EchoRadar --model models\v4-candidate`. Confirm the package version is logged and events include onset, detected, delivered, source, scene, and stream fields.
6. Temporarily rename or corrupt a copy of the model package. Confirm capture continues while recognition reports that it is paused.
7. Compare at least one recorded WAV through Python v4 inference and the live/native event trace before judging latency or accuracy.

The first six checks validate runtime wiring and recovery. Production promotion still requires the locked support, accuracy, suppression, export-parity, and latency gates documented in the training guide.
