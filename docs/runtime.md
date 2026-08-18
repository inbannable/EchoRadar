# Runtime

EchoRadar captures the selected Windows render endpoint through WASAPI loopback at 48 kHz interleaved stereo float32. Playback stays on the user's selected output device.

The capture callback only copies PCM to the bounded SPSC ring and updates levels. The processing thread consumes PCM, advances streaming features, runs recognition, retains a short scene history, and schedules direction inference. Endpoint changes, restarts, overflow, and excessive backlog create a new stream generation; all causal recognition state and pending direction scenes are reset at that boundary.

Recognition uses the fixed `stereo-onset-v4` contract: 1024-point FFT, 240-sample hop, 64 mel bands, five input planes, a 128-frame context, gunshot/footstep onset heads, and self/remote/unknown source heads. Package metadata controls thresholds, peak lookahead, spacing, onset correction, scene activity, and self suppression.

When `--direction-model` is supplied, accepted recognition events within 120 ms share one 12,304-sample scene. The direction model consumes `[1,5,48,64]` and returns up to three exchangeable 3D source vectors. Direction can be enabled independently for gunshots and footsteps. Missing audio or a model failure produces an explicit scene status and never invokes a fallback.

The HUD receives one update per scene and renders every returned source with confidence-derived uncertainty. The control UI displays the same `DirectionSceneResult`, including all sources, and can play the shared scene WAV.

Each logged event has `schema_version: 2` and retains event identity, stream generation, recognition timing and confidence, scene bounds and delivery timing, recognition/direction model versions, enabled classes, inference timing and dimensions, clip path, status, and the complete source array.

Settings use schema 3. Schema-2 files preserve active audio-profile, direction class-enable, HUD, UI-scale, and logging fields; retired tuning keys are ignored. Migration never deletes `direction-calibration.tsv`.
