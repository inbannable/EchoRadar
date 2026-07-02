# Milestone 6: Dataset Pipeline and Offline Analysis Platform

## Scope

Milestone 6 focuses on building the data loop, not improving detector logic:

- Keep `GunshotEventDetector` unchanged
- Build event-centric dataset capture pipeline
- Export audio + features + metadata for offline analysis

## Pipeline

`AudioCapture -> STFTProcessor -> FeatureExtractor -> GunshotEventDetector -> dataset_recorder`

Recorder stages:

1. Capture detector decisions (`Peak`, `Accepted`)
2. Wait for full 200 ms post context
3. Pull 400 ms PCM window from `AudioHistoryBuffer` (200 ms pre + 200 ms post)
4. Pull matching feature rows from `FeatureHistoryBuffer`
5. Persist asynchronously to disk as:
   - `audio.wav`
   - `features.csv`
   - `metadata.json`

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
