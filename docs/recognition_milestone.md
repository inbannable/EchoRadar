# CS2 Sound Recognition Milestone

> This document records the native v3 milestone. New model preparation and
> CPU training use the v4 contract documented in
> [`ml/TRAINING_V4.md`](../ml/TRAINING_V4.md). No v4 model has been trained or
> promoted yet; the v4 runtime is now connected to the native application for
> experimental candidate validation as documented in
> [`audio_capture_v4_runtime.md`](audio_capture_v4_runtime.md).

## Implemented contracts

The recognition layer exposes three user-facing classes: `Gunshot`, `Footstep`, and `Mechanical`. `SoundRecognizer` accepts streaming 48 kHz interleaved stereo float PCM and returns zero or more overlapping `SoundEvent` objects containing class, onset/end sample, confidence, and model version.

V3 packages emit independent, immediate 50 ms pulses. Each event carries calibrated acoustic onset, fixed end sample, delivery-time `detectedSample`, confidence, and model version. Rearming requires one inference below the class rearm threshold plus expiry of the 40/60/80 ms gunshot/footstep/mechanical refractory period.

`stereo-pcen-v2` computes the left and right spectra separately and averages power, preventing directional phase cancellation. Its two planes are causal PCEN (`s=0.025`, `alpha=0.98`, `delta=2`, `r=0.5`, `epsilon=1e-6`) and absolute mel energy clipped to `[-100, 0]` dB. It retains 48 kHz, a 1024-sample symmetric Hann FFT, 512-sample hop, 64 mel bands, and 96 causal frames. Numeric C++/NumPy golden vectors cover streaming boundaries, channel swaps, directional phase, and gain changes. `logmel-v1` remains supported unchanged.

A model package contains `recognizer.onnx` and a flat `model.json`. Metadata fixes the class order, preprocessing dimensions, inference stride, thresholds, hysteresis, refractory periods, model version, and SHA-256. The native loader fails closed on incompatible metadata or model bytes.

## Data pipeline

The Python package reads PCM8/PCM16 mono/stereo WAV, deterministically resamples to 48 kHz stereo, and removes exact duplicate copies before splitting. Gunshot/mechanical assets remain grouped by weapon family and footsteps by surface; background assets split by canonical source file. The split is performed before augmentation.

Continuous session generation provides:

- Exact JSONL event timelines and versioned session metadata.
- Simple scenes with isolated events at 10-24 dB SNR.
- Complex scenes with -10-20 dB SNR, guaranteed target overlap, hard negatives, gain changes, low-pass occlusion, reverberation, and compression.
- A reproducible 160/40/40 corpus command with 30-second sessions, 25% ambient-only coverage, whole-session gain from -30 to +6 dB, and balanced self/remote rendering.
- Steam Audio v4.8.1 offline rendering with HRTF bilinear interpolation, direct effects, occlusion/transmission, and short room/corridor tails. The SDK archive and checksum are pinned; generation fails closed if the renderer is unavailable or mismatched.
- Activity trimming, model-context warm-up, and at least one example of every target class per session.

The evaluator matches event onsets within 150 ms, reports per-class precision/recall/F1, false alerts per minute and latency, preserves probability traces, and exports a two-second review clip around every miss and false positive.

## Model and runtime

V3 uses a compact causal depthwise CNN and unidirectional GRU with an objectness head and three independent class heads. Export multiplies objectness by each class probability, preserving the three-value output contract from a fixed `[1, 2, 96, 64]` input. Targets are weighted onset pulses over three frames (`1.0, 0.5, 0.25`). Training combines focal objectness/class losses, a 0.25 single-class cross-class margin, balanced ambient/target batches, and one mined hard-negative retraining pass. Threshold selection first satisfies ambient and wrong-class constraints, then maximizes recall and minimizes duplicates and onset error.

The native backend is enabled with `ECHORADAR_ENABLE_ONNX=ON`. ONNX Runtime executes with one intra-op thread and full graph optimization. The live monitor reports current probabilities and inference time; the offline evaluator uses identical `SoundRecognizer` code.

## Real-game workflow

The timeline reviewer displays a movable ten-second waveform and log-mel spectrogram. Predictions may seed the timeline, but human changes are recorded separately with `reviewed`, `source`, and `uncertain` fields. Saves are staged and atomically replace the labels file.

Replay parsing remains a bounded experiment because no CS2 replay has been supplied. Recognition validation does not depend on it: synchronized replay screen capture plus loopback audio is the fallback. Stereo audio, replay/video context, CS2 sound settings, capture route, map, and session identity must be retained for later localization and reproducibility.

## Completion state

Infrastructure implementation is complete and a v3 one-epoch smoke package passes ONNX Runtime parity. This is not an accuracy result. Full 160/40/40 rendering, training, locked evaluation, native wiring gates, and gameplay validation remain required before promotion. Both existing models remain unchanged.
