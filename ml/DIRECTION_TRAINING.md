# 3D multi-source direction training

The direction pipeline is separate from the v4 onset recognizer. It renders
fixed 256.333 ms scenes, extracts the shared five-plane stereo representation,
trains a compact class-wise Multi-ACCDOA network, and exports a dedicated
`direction.onnx` package. The public runtime result is an unordered set of zero
to three directions; gunshot/footstep classes remain internal so gunshots can
be removed in footstep-only mode.

Generation and training are intended for Windows. The generator refuses a
renderer whose `--version` output is not
`echoradar-steam-audio-renderer v4.8.1`. The Mac checkout can run generator
unit tests, inspect manifests, and build the non-ONNX native path, but it does
not contain the Steam Audio SDK runtime or the locked PyTorch/ONNX environment.

## 1. Generate scenes

Prepare the grouped asset manifest first, then build the pinned renderer as
described in `TRAINING_V4.md`. For the 2,000/250/500 smoke corpus:

```powershell
echoradar-ml direction-mixtures `
  --manifest ml\generated\v4\asset-manifest.csv `
  --asset-root sounds `
  --output ml\generated\direction-smoke `
  --steam-audio-renderer build\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe `
  --preset smoke
```

Use `--preset full` for 100,000 train, 10,000 development, and 20,000 locked
test scenes. Explicit `--train-count`, `--dev-count`, and `--test-count`
values override a preset for small engineering runs.

Each WAV has exactly 12,304 samples at 48 kHz. This produces 48 feature frames
with `1024 + (48 - 1) * 240` samples. Targets are scheduled at exact
10%/15%/35%/40% proportions for counts 0/1/2/3. Same-count class combinations
cycle evenly. Sources are independently spatialized before summing, separated
by at least 15 degrees on the sphere, and 20% of locked-test scenes form the
dedicated 15–30 degree stratum. Half of multi-source scenes use coincident
onsets; the rest use deterministic 0–80 ms jitter.

`direction-scenes.jsonl` records the seed, profile, WAV checksum, renderer,
targets, distractors, source assets/groups, onsets, gains, world/rendered
directions, distance, and propagation parameters. Only the shared capture/EQ
chain and final anti-clipping gain are applied after summing, so inter-source
levels are preserved.

## 2. Cache and train

```powershell
echoradar-ml direction-cache `
  --manifest ml\generated\direction-smoke\direction-scenes.jsonl `
  --audio-root ml\generated\direction-smoke `
  --output ml\generated\direction-smoke\feature-cache

echoradar-ml direction-train `
  --cache ml\generated\direction-smoke\feature-cache `
  --output models\direction-candidate `
  --epochs 20 `
  --batch-size 64
```

The cache stores `[5,48,64]` features and `[2,3,3]` target vectors. It rejects
real locked manifests, incompatible preprocessing, changed WAV hashes, and
wrong scene lengths. Float16 is the default feature-cache storage format;
training converts batches to float32.

The model is a CPU-oriented convolutional/bidirectional-GRU network guarded by
a two-million-parameter limit. Class-wise ADPIT duplicates a single source
across all tracks, duplicates either member of a two-source target, and compares
all three-source permutations independently per class. Sampling increases the
weight of two- and three-source scenes and the close-source stratum. Channel-swap augmentation
negates ILD, imaginary coherence, and target `x`, preserving elevation and
front/back coordinates.

`direction-checkpoint.pt` is replaced after every completed epoch. Checkpoints
contain optimizer and Python/NumPy/PyTorch RNG state; resume with:

```powershell
echoradar-ml direction-train `
  --cache ml\generated\direction-smoke\feature-cache `
  --output models\direction-candidate `
  --epochs 20 `
  --resume models\direction-candidate\direction-checkpoint.pt
```

## 3. Package, parity, and gates

The package contains:

- `direction.onnx` with input `[1,5,48,64]` and output `[1,2,3,3]`;
- `direction.json` with the model SHA-256, preprocessing/coordinate contract,
  exchangeable track order, per-class development thresholds, elevation
  bounds, duplicate policy, and confidence-to-p90 uncertainty table;
- `direction-parity.npz`, `direction-checkpoint.pt`, and
  `training-summary.json`.

Verify the export and evaluate the locked test set:

```powershell
echoradar-ml direction-parity --model models\direction-candidate

echoradar-ml direction-evaluate `
  --manifest ml\generated\direction-smoke\direction-scenes.jsonl `
  --audio-root ml\generated\direction-smoke `
  --model models\direction-candidate `
  --split test `
  --gate synthetic `
  --require-gates `
  --output ml\runs\direction-candidate.json
```

Add `--footstep-only` to exercise disabled-gunshot filtering. ONNX parity fails
above `1e-5`. Synthetic and real promotion thresholds are encoded separately;
`--gate auto` selects the gate from the manifest schema.

## 4. Runtime

Start the app with both packages:

```powershell
.\build\src\app\Release\EchoRadar.exe `
  --model models\v4-candidate `
  --direction-model models\direction-candidate
```

The app anchors a scene 40 ms before the earliest accepted event, joins events
whose onsets fall within the next 120 ms, waits for all 12,304 samples, and runs
one localization inference. Every member event receives the same `scene_id`
and source array; the HUD receives the scene only once. Stream discontinuities
clear pending scenes. The old single-source mapper is never an implicit
fallback and is available only through `--legacy-direction-diagnostic`.

See `docs/cs2_3d_validation.md` before promoting a package on real CS2 audio.
