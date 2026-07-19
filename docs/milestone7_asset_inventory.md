# Milestone 7: Extracted Asset Inventory

## Scope

Phase 1 of the asset-based workflow turns a local extracted WAV tree into deterministic metadata. It does not copy, resample, rename, delete, or otherwise modify source audio.

The source assets remain local under an ignored `pilot-datasets/` path and must not be committed or redistributed with EchoRadar.

## Command

```powershell
.\build\tools\asset_inventory\Release\asset_inventory.exe `
  --asset-root "Z:\CODE\EchoRadar\pilot-datasets\sounds" `
  --output-dir "Z:\CODE\EchoRadar\pilot-datasets\asset-inventory"
```

## Outputs

- `asset_manifest.csv`: all valid and non-hidden WAV candidates
- `review_needed.csv`: invalid WAVs and classifications below 0.80 confidence
- `summary.json`: input, quality, duplicate, duration, and label counts

Each manifest row records the source-relative path, SHA-256, stable content-derived asset ID, four-class label, subtype, weapon, footstep surface, distance, source group, classification rule and confidence, input WAV format, frame count, duration, peak, RMS, duplicate relationship, inclusion state, and error text.

## Classification rules

The detector-facing taxonomy is:

- `gunshot`: near, distant, suppressed, and unsuppressed firearm reports
- `footstep`: footsteps and landing variants grouped by surface
- `mechanical`: reload, switch, draw, magazine, bolt, slide, pump, attachment, zoom, and device-handling sounds
- `other`: projectile effects, ricochets, melee, explosions, player sounds, and remaining non-target audio

Folder names alone are not labels. Firearm folders contain both reports and mechanical operations, so filename tokens refine the result. The manifest preserves the exact rule that produced each classification.

## Ignored content

The scanner ignores non-WAV files, `.DS_Store`, and any path component beginning with `._`. These `._` entries are macOS AppleDouble metadata and are not valid audio.

## Current local inventory

The current library scan reports:

| Item | Count |
|---|---:|
| Discovered files | 2,250 |
| Ignored metadata/non-WAV files | 1,156 |
| Valid WAV files | 1,094 |
| Invalid WAV files | 0 |
| Exact duplicate copies | 35 |
| Gunshot | 127 |
| Footstep | 395 |
| Mechanical | 283 |
| Other | 289 |
| Low-confidence review | 2 |

The two review rows are `weapons/negev/negev_clean_01.wav` and `weapons/negev/negev_clean_02.wav`. Both are provisionally classified as gunshots.

## Completion gate

Phase 1 is complete when:

1. The local asset tree is excluded from Git.
2. Repeated inventory runs produce byte-identical reports.
3. Every valid source WAV is represented once in the manifest.
4. Invalid and hidden metadata files cannot enter the training corpus.
5. Exact duplicate bytes are linked to a canonical source path.
6. No duplicate hash receives conflicting labels.
7. Automated tests validate SHA-256, PCM8/PCM16 parsing, classification, ignored metadata, duplicates, and report export.

## Next phase

Phase 2 adds reusable PCM loading, mono-to-stereo conversion, and resampling to 48 kHz. Source grouping and deterministic train/development/test splitting must happen before any augmented or synthetic samples are generated.
