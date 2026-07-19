# Milestone 6: Dataset Pipeline and Offline Analysis Platform

## Scope

Milestone 6 focuses on building the data loop, not improving detector logic:

- Keep `GunshotEventDetector` unchanged
- Build event-centric dataset capture pipeline
- Export audio + features + metadata for offline analysis

## Pipeline

`AudioCapture -> STFTProcessor -> FeatureExtractor -> GunshotEventDetector -> dataset_recorder`

Recorder stages:

1. Capture each detector peak and attach its final accepted/rejected decision
2. Wait for full 200 ms post context
3. Pull 400 ms PCM window from `AudioHistoryBuffer` (200 ms pre + 200 ms post)
4. Pull matching feature rows from `FeatureHistoryBuffer`
5. Persist asynchronously into a private staging directory as:
   - `audio.wav`
   - `features.csv`
   - `metadata.json`
6. Atomically publish the complete event directory

Recorder sessions use collision-resistant event IDs and never overwrite an
existing event. Failed writes are removed from the staging area instead of
leaving partial dataset entries.

## New core components

### AudioHistoryBuffer

- 3-second rolling stereo PCM cache
- Supports absolute-sample window extraction
- Supports time-centered extraction (`center ± window`)

### FeatureHistoryBuffer

- 3-second rolling feature cache
- Stores per-frame `AudioFeatures` + detector score/confidence
- Supports time-range extraction for CSV export

## Dataset layout

Recorder creates:

`dataset/gunshot`, `dataset/footstep`, `dataset/reload`, `dataset/switch`,
`dataset/ambient`, `dataset/unknown`

Current auto-save target:

`dataset/unknown/<event_id>/audio.wav|features.csv|metadata.json`

Each physical detector candidate produces one clip. `metadata.json` records
the final detector decision, recorder session, wall-clock timestamp, and human
review status. Manifest export covers all six labels, not only `unknown`.

## Recorder UI

- Live statistics:
  - Total Events
  - Saved Events
  - Discarded Events
  - Disk Usage
  - Recording Time
- Recent events (last 10)
- Waveform viewer
- Spectrogram viewer
- Replay selected event
- Manual ambient snapshot
- Manifest export (`dataset/manifest.csv`)

## Dataset Studio review flow

- Unreviewed clips are shown by default
- Keys `1` through `6` label/review the current selection
- Auto-advance can select and optionally play the next clip
- Review state is independent from the label, so a clip may remain `unknown`
  while still being marked as reviewed
- Manifest export, soft delete, restore, and undo route through `DatasetManager`
- Recorder and Dataset Studio both accept `--dataset-root <path>` so capture and
  review can share a non-default dataset location
