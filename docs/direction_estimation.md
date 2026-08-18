# Direction estimation and overlay

EchoRadar’s primary direction path is scene-level and model-backed. It returns
an unordered set of zero to three azimuth/elevation estimates with confidence
and calibrated p90 angular uncertainty. Footstep and gunshot classes are used
inside the model only, allowing gunshots to be filtered when footstep-only mode
is selected.

The prior single-event v2 mapper and guided calibration remain for one
compatibility cycle as an explicit diagnostic. They never run because a 3D
package is missing, corrupt, or produces an inference error. Start that path
only with `--legacy-direction-diagnostic`; start normal localization with
`--direction-model <package-directory>`.

## Scene runtime flow

1. WASAPI loopback writes sequential 48 kHz stereo frames to the v4 recognizer
   and a three-second history buffer.
2. The first accepted event starts a scene 40 ms before its onset. Other
   accepted events with onsets through exactly 120 ms after that first onset
   join the same scene.
3. The coordinator waits until all 12,304 samples (256.333 ms) are available.
   It extracts the shared PCEN, absolute-energy, ILD, and real/imaginary
   coherence planes as `[1,5,48,64]` and performs one ONNX call.
4. The `[1,2,3,3]` Multi-ACCDOA output is thresholded per class. Disabled
   classes are removed; same-class tracks merge only below 7.5 degrees; the
   three strongest remaining tracks become the public source set.
5. Every recognition event in the group receives the shared `sceneId` and
   compatibility primary/secondary values. The HUD receives one scene marker,
   preventing duplicate visualization.

Capture discontinuities, endpoint changes, and backlog resets clear both audio
history and pending scene groups. A scene never spans two stream generations.
If audio, package validation, or inference fails, the status is logged rather
than invoking the legacy mapper.

## Package contract

`direction.json` pins the ONNX SHA-256 and these runtime invariants:

- 48 kHz stereo, FFT 1024, hop 240, 64 mel bands, 48 frames, five planes;
- class order `gunshot,footstep` and three exchangeable tracks per class;
- coordinates `x=right`, `y=up`, `z=forward` with elevation in −60° to +60°;
- per-class development thresholds, a 7.5° duplicate boundary, and at most
  three public sources;
- the exact PCEN parameters and monotonic confidence-to-p90 table.

The native loader rejects a missing model, checksum mismatch, incompatible
tensor/preprocessing contract, invalid threshold, or non-monotonic uncertainty
table. ONNX output must be finite and shaped `[1,2,3,3]`.

## HUD and logs

The HUD is a separate topmost, non-activating, click-through Windows window. It
follows the CS2 client rectangle in borderless mode and draws up to three
uncertainty arcs. Each source’s elevation moves the marker vertically; an
up/down chevron and signed degree text make the sign explicit. Confidence
controls opacity and the development-calibrated p90 error controls arc width.
`Ctrl+Alt+O` hides or restores the HUD globally.

Each event JSONL record retains the compatibility direction fields and adds the
shared scene ID/bounds, enabled-class mask, complete source array, direction
model/preprocessing versions, feature-frame count, and inference latency. The
exact shared scene PCM is stored once under
`sessions/clips/<session-id>/scene-<id>.wav`.

## Why profiles require independent real validation

Steam Audio applies a different pair of HRTF filters to every point source,
encoding interaural timing, level, and spectral cues before sources mix. CS2
can further change these cues through EQ, L/R isolation, perspective
correction, device processing, and Windows spatial enhancements. Consequently,
successful validation for one profile does not transfer to another.

The synthetic pipeline records its EQ/isolation/capture profile and both world
and rendered directions. Real perspective-corrected data uses the projected
camera ray as the rendered/perceived target while retaining world coordinates
for audit. See [the controlled validation protocol](cs2_3d_validation.md).

## Generate and train

The single-source command is preserved for mapper/calibration regressions:

```powershell
echoradar-ml direction-corpus `
  --source-root sounds `
  --output ml\generated\direction-single `
  --steam-audio-renderer build\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe
```

The production candidate uses `direction-mixtures`, `direction-cache`,
`direction-train`, `direction-parity`, and model-backed
`direction-evaluate`. Full commands, corpus sizes, resume behavior, package
contents, and locked gates are in [the direction training guide](../ml/DIRECTION_TRAINING.md).

## Legacy diagnostic

The diagnostic searches an event-local broad window for its strongest
smoothed-RMS peak, extracts the v2 GCC/ILD/spectral summary, and produces one
24-bin azimuth distribution plus an optional front/rear mode. It remains useful
for listening to selected clips and comparing old calibration profiles, but it
cannot represent overlapping same-class sources or elevation and is not a
runtime fallback.
