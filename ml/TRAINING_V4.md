# EchoRadar v4 CPU training

The v4 pipeline is ready to prepare data and train a candidate model. It emits
`gunshot` and `footstep` onsets, treats every other sound as a named hard
negative, learns `self` / `remote` / `unknown` source hints, and calibrates a
quiet/busy footstep policy. It does not train a direction estimator.

## 1. Environment

On the Windows machine that will run training:

```powershell
py -3.12 -m venv ml\.venv
.\ml\.venv\Scripts\python.exe -m pip install --upgrade pip
.\ml\.venv\Scripts\python.exe -m pip install -r ml\requirements-lock.txt
.\ml\.venv\Scripts\python.exe -m pip install -e ml
```

The lock file installs CPU-capable PyTorch, ONNX, and ONNX Runtime. No CUDA
code path is used. `--threads 0` lets PyTorch select the CPU thread count;
specify a positive count when the workstation must remain responsive.
Reserve roughly 10 GB of free space for the default synthetic corpus, three
training-gain feature caches, checkpoints, and evaluation artifacts. CPU
training is expected to take hours rather than minutes; checkpoints make it
safe to interrupt between epochs.

## 2. Prepare extracted assets

```powershell
echoradar-ml prepare-assets `
  --asset-root sounds `
  --output ml\generated\v4\asset-manifest.csv
```

This command validates and hashes every PCM WAV, removes duplicate copies from
the split, converts the detector taxonomy to two targets plus named negatives,
measures the first audible sample after 48 kHz band-limited resampling, and
splits weapon families and footstep surfaces without leakage. The supplied
tree should report 1,094 WAVs, 35 duplicates, 127 gunshots, 395 footsteps, 572
negatives, and two review rows (`negev_clean_01/02.wav`). Review or accept those
two rows listed in `asset-manifest.review.csv`; if confirmed, set the matching
rows' `review_status` to `accepted` in the main manifest before treating the
corpus as final.

## 3. Build synthetic sessions

The locked spatial corpus requires the repository's pinned Steam Audio v4.8.1
renderer. Build it as described in the root README, then run:

```powershell
echoradar-ml corpus `
  --manifest ml\generated\v4\asset-manifest.csv `
  --asset-root sounds `
  --output ml\generated\v4\sessions `
  --steam-audio-renderer build\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe
```

For a non-spatial smoke corpus, use `echoradar-ml mixtures` without a renderer.
Do not use that shortcut for production source-suppression results.

V4 sessions contain exact audible onsets, explicit 60 ms gunshot pairs, 100 ms
footstep pairs, quiet/busy scenes, local-SNR scaling, hard-negative transients,
capture gain, compression, filtering, reverberation, and balanced self/remote
targets.

## 4. Add real gameplay recordings

Record 48 kHz stereo loopback audio with synchronized video. The annotation
tool writes canonical 48 kHz sample indices even when the source WAV has a
different rate:

```powershell
echoradar-ml annotate `
  --wav recordings\session01.wav `
  --labels recordings\session01.labels.jsonl
```

Keys: `1` gunshot, `2` footstep, `L` self, `R` remote, `N` unknown, `U`
uncertain, `Delete` remove, `Space` preview, and `S` save. Every retained event
must be reviewed. Uncertain events are preserved but excluded from training and
evaluation.

Import each complete recording into exactly one split. Keep every capture day
and gameplay session in one split; never divide one recording across train,
development, and test.

```powershell
echoradar-ml import-real `
  --wav recordings\session01.wav `
  --labels recordings\session01.labels.jsonl `
  --output ml\generated\v4\sessions `
  --split train `
  --session-id session01 `
  --map de_dust2 `
  --capture-day 2026-08-08 `
  --audio-settings "48k; headphones; fixed CS2 mix"
```

If labels came from another tool and use different sample coordinates, pass
`--label-sample-rate` explicitly; the importer converts them to canonical 48
kHz indices.

Audit support at any time:

```powershell
echoradar-ml audit --sessions ml\generated\v4\sessions
```

The production gate requires at least 300 locked remote events and 200 locked
self events per emitted class, one hour of target-free audio, three maps, and
three capture days. Evaluation also requires 100 quiet remote footsteps, 100
busy remote gunshots, and 100 rapid-gunshot pairs before those gates are
conclusive. Training itself only requires nonempty train and
development sessions, so small smoke runs remain possible.

## 5. Cache and train on CPU

Feature caching is separate and restartable. It stores session features once;
the trainer reads overlapping windows on demand instead of expanding the whole
corpus in RAM. When both domains are present, batches are balanced across
real/synthetic sessions as well as positive/negative onset windows so a large
synthetic corpus cannot drown out gameplay evidence.
Threshold, scene, timing, and self-suppression calibration use reviewed real
development sessions whenever any are present; synthetic development sessions
are the fallback for smoke training only.

```powershell
echoradar-ml cache `
  --sessions ml\generated\v4\sessions `
  --output ml\generated\v4\feature-cache

echoradar-ml train `
  --sessions ml\generated\v4\sessions `
  --cache ml\generated\v4\feature-cache `
  --output models\v4-candidate `
  --epochs 20 `
  --batch-size 64 `
  --threads 0
```

Training writes `training-checkpoint.pt` after every epoch. Resume an interrupted
run with `--resume models\v4-candidate\training-checkpoint.pt`. The final
package contains the checkpoint, ONNX model, SHA-256 metadata, calibrated
quiet/busy thresholds, onset offsets, self-suppression threshold, parity
fixture, and training summary.

## 6. Verify the candidate

```powershell
echoradar-ml parity `
  --model models\v4-candidate `
  --fixture models\v4-candidate\onnx_parity.npz

echoradar-ml evaluate `
  --sessions ml\generated\v4\sessions `
  --model models\v4-candidate `
  --output ml\runs\v4-candidate `
  --require-gates
```

Evaluation saves score traces, source probabilities, activity traces,
predictions, error clips, per-session metrics, and the complete acceptance
report. A small or synthetic-only corpus is expected to fail the real-data
support gates; that is a data-readiness result, not a tooling failure.

## Current boundary

The repository is prepared through CPU training, ONNX export, calibration,
Python evaluation, matching native C++ v4 features/inference/post-processing,
and experimental main-application integration. No v4 model has been trained or
promoted. A first candidate may be loaded immediately from
`models\v4-candidate` for capture, parity, latency, and real-game engineering
validation; this does not waive the locked data and accuracy gates required for
production promotion. Legacy v1/v3 diagnostic paths remain available and are
not used as an implicit fallback by the main v4 runtime.
