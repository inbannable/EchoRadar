# Milestone 5: Multi-stage GunshotEventDetector

## Overview

`GunshotEventDetector` has been upgraded from a single threshold state machine into a
multi-stage pipeline:

`AudioFeatures -> Candidate Score -> Peak Picking -> Temporal Validation -> False Positive Filter -> Confidence Gate -> GunshotEvent`

`FeatureExtractor` remains unchanged.

## Stage 1: Candidate score

Each frame gets a high-recall candidate score from:

- `energyRise`
- `spectralFlux`
- `hfEnergyRatio`
- `transientScore`
- a small centroid lift term

The raw score is normalized with EMA baselines (`score` + `logEnergy`) so stable
background audio does not keep firing candidates.

## Stage 2: Peak picking

Only local maxima can become candidates:

- local rule: `mid >= left && mid >= right && (mid > left || mid > right)`
- minimum trigger: `mid >= triggerThreshold`
- adjacent plateau maxima are merged to one pending peak

This avoids repeated triggers during sustained high scores.

## Stage 3: Temporal validation

For each peak, detector builds a local event envelope and validates:

- total duration (`minEventFrames .. maxEventFrames`)
- rise duration (`maxRiseFrames`)
- decay duration (`maxDecayFrames`)
- peak width (`maxPeakWidthFrames`)
- prominence / rise slope / decay slope

Slow, wide, long events are rejected before classification.

## Stage 4: False positive filter

Candidate peaks are checked with multi-feature rules (not single-feature):

- energy rise
- spectral flux
- transient score
- high-frequency ratio
- spectral centroid
- spectral flatness
- duration / width context from temporal stage

Footstep-like / handling-like / broad-noise-like patterns are filtered out.

## Stage 5: Confidence gate

Accepted candidates get `confidence` in `[0, 1]` from:

- candidate peak score
- impulse features (`energyRise`, `flux`, `transient`, `hf`)
- spectral cues (`centroid`, `flatness`)
- temporal compactness and peak sharpness

Only candidates with `confidence >= minConfidence` emit `GunshotEvent`.

## Event splitting for burst fire

Burst shots (AK/M4) are split by design using:

- local-peak-only candidate generation
- `minPeakIntervalFrames`
- `minEventSeparationFrames`

So repeated peaks in one high-energy region can still become separate events.

## Debug output

Detector exposes per-candidate decisions:

- `Peak`
- `RejectedTemporal`
- `RejectedFalsePositive`
- `RejectedConfidence`
- `Accepted`

This is consumed by the visualizer for stage-by-stage explanation.
