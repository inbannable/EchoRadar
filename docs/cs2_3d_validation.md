# Controlled CS2 3D direction validation

Real promotion uses a locked Workshop capture corpus. Synthetic results alone
cannot validate CS2 perspective correction, the output device, Windows spatial
enhancements, or the complete capture chain.

## Map and coordinate contract

Build a private Workshop map in Hammer with a fixed listener spawn and a visible
forward-axis marker. Treat listener right as `+x`, up as `+y`, and forward as
`+z`. Place named `point_soundevent` emitters on a measured 3D lattice covering
all azimuth quadrants, elevation bands below −20°, −20° through +20°, and above
+20°, and multiple distances. Use map I/O or a deterministic script to trigger
balanced one-, two-, and three-emitter footstep/gunshot combinations. A trigger
record must store the emitter entity names, listener transform, emitter world
coordinates, and engine timestamp before it fires.

With perspective correction disabled, derive labels from listener-relative
world vectors. With it enabled, also record the screen-projected camera ray and
use that rendered/perceived ray as `rendered_direction`; retain the world value
for auditing. Do not mix the two policies in one audio-profile result.

## Capture protocol

Capture WASAPI loopback at 48 kHz stereo and record the following immutable
profile fields with every session:

- CS2 EQ and L/R isolation;
- perspective-correction state and display aspect ratio;
- output endpoint/device;
- Windows spatial-enhancement state;
- room, capture session/day, map revision, listener transform, and script seed.

Use three acoustic rooms and three independent capture sessions. The locked set
must contain at least 1,200 scenes, including at least 200 two-source and 200
three-source scenes, and all three elevation bands. Split reviewed train/dev
material before recording or designating the locked set. The locked files and
their source sessions must never be passed to `direction-cache` or
`direction-train`.

Slice each evaluated event group to the same 12,304-sample window used by the
runtime. The locked JSONL uses `schema_version: "direction-real-scenes-v1"`
and the common synthetic scene fields (`relative_path`, `split`, `targets`,
`target_count`, sample/FFT/hop dimensions). Add `room`, `capture_session`,
`profile`, `listener_transform`, emitter coordinates, and perspective labels.
Set `split` to `test`. `rendered_direction` is always the scoring target.

## Evaluation and promotion

```powershell
echoradar-ml direction-evaluate `
  --manifest recordings\direction-locked\direction-real-scenes.jsonl `
  --audio-root recordings\direction-locked `
  --model models\direction-candidate `
  --split test `
  --gate real `
  --require-gates `
  --output ml\runs\direction-real-locked.json
```

The real gate verifies locked support, room/session/elevation coverage, exact
count accuracy, source F1 within 30°, median/p90 great-circle error, elevation
p90, disabled-gunshot inclusion, ONNX p95 inference time, and the fixed scene
window delivery bound. Run a separate report for every audio profile. A profile
is experimental until its own report passes; success for one endpoint or CS2
audio configuration does not transfer to another.

V1 does not use pseudo-labeling. Reviewed real train/dev scenes may later be
used for a documented fine-tune, but locked real scenes remain evaluation-only.
