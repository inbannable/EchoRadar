# Milestone 5: GunshotEventDetector

## Overview

Milestone 5 adds a rule-based, event-level gunshot detector that consumes per-frame
`AudioFeatures` and emits `GunshotEvent` objects.

Pipeline:

`AudioCapture -> AudioRingBuffer -> STFTProcessor -> FeatureExtractor -> GunshotEventDetector`

## Feature dependencies

The detector uses these `AudioFeatures` fields:

- `logEnergy`
- `energyRise`
- `spectralFlux`
- `hfEnergyRatio`
- `transientScore`
- `leftRightBalance`
- `spectralCentroid`
- `spectralFlatness`

`FeatureExtractor` computes the frame-to-frame state needed for `energyRise`,
`spectralFlux`, and the transient baseline.

## Candidate score

The detector computes a frame score with a simple weighted rule:

`score = f(energyRise, spectralFlux, hfEnergyRatio, transientScore)`

Implementation notes:

- Each input feature is clamped to a stable range.
- A small EMA baseline is maintained for score normalization.
- `logEnergy` is used as a soft gate so steady background noise does not keep
  retriggering candidates.

## State machine

The detector uses three states:

- `Idle`
- `InCandidate`
- `Cooldown`

### Idle

Waits for `score >= triggerThreshold`.

### InCandidate

Tracks:

- `startFrame`
- `peakFrame`
- `endFrame`
- peak score and peak-frame auxiliary features

The candidate stays open while the score remains above `releaseThreshold`.
Short dips are merged using `maxMergeGapFrames`.

### Cooldown

Suppresses immediate retriggers for `refractoryFrames`.

## Merge / cooldown behavior

Nearby bursts are merged by allowing a short gap below release before finalizing
the event. This prevents one gunshot from being split across adjacent STFT frames.

After finalization, the detector enters cooldown so the same shot does not fire
multiple events.

## Limitations

- It is a baseline rule-based detector, not a learned classifier.
- Loud non-gunshot transients may still trigger it.
- Footsteps, explosions, and unusual environmental sounds can cause false positives.
- Performance depends on the quality of the upstream feature extraction and audio mix.

## Next step

Milestone 6 will use completed gunshot events to extract spatial cues for direction
estimation, such as:

- event-window isolation
- ILD
- GCC-PHAT / coherence

The detector itself stays focused on event detection only.
