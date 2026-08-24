# EchoRadar v2.0.0-research release notes

Status: release candidate. Do not publish or create the release tag until the
real-device and visual gates in [evaluation.md](evaluation.md) are recorded for
the exact release commit.

## Current verification evidence

The local working tree has passed 85/85 native tests in both Debug and Release
and 31/31 Python tests. A limited local smoke check also confirmed fail-closed
directional behavior on a 2-channel/48 kHz endpoint, CJK endpoint-name
rendering, focused-dashboard HUD suppression, and HUD display after focus moved
away. These results must be repeated on the exact release commit and do not
authorize a tag or publication.

## Highlights

- Native Windows 48 kHz 5.1/7.1 directional radar with exact endpoint speaker
  mask validation and no stereo direction approximation.
- Original 24-sector frequency-domain energy-vector DSP with continuous,
  event, and combined modes.
- `All`, `Footsteps`, `Gunshots`, and editable `Custom` spectral-emphasis
  curves; these names do not imply semantic classification.
- LFE-free, bounded stereo recognition downmix with unchanged recognition and
  direction model-package formats.
- Tactical-dark single dashboard, resumable setup, recent-event timeline, and
  original click-through/editable per-display HUD.
- Settings schema 4 in `%LOCALAPPDATA%\EchoRadar\v2` and JSONL session schema 3.
- Incremental multichannel WAVEFORMATEXTENSIBLE recording with bounded memory.

## Compatibility

Directional radar requires an endpoint whose actual Windows shared-mode mix
format is 48 kHz and whose explicit `WAVEFORMATEXTENSIBLE` mask is one of the
supported 5.1-back, 5.1-side, or 7.1 layouts. Client-side sample-rate or channel
conversion does not make an endpoint eligible. Stereo and 44.1 kHz endpoints
remain available to recognition and settings.

EchoRadar v1 remains frozen at branch `codex/v1-maintenance` and annotated tag
`echoradar-v1.0.0` (commit `51f130a`).

## Known limitations

- Only azimuth is reported; elevation is not inferred.
- Radar peaks are energy maxima, not separated or identified physical sources.
- Opposing same-frequency energy can cancel in the energy vector.
- Room simulation, reflections, game mixes, endpoint effects, and virtual
  surround can reduce bearing accuracy.
- Real 5.1/7.1 accuracy and bearing, end-to-end latency, DSP timing,
  capture-callback allocation instrumentation, and the complete display/DPI
  matrix remain unmeasured mandatory publication gates.

## Licensing and provenance

This release is Apache-2.0 research software. The license grants patent rights
from contributors for their contributions; it does not grant rights under
third-party patents. See [NOTICE](../NOTICE),
[algorithm provenance](algorithm-provenance.md), and the disclosure for
[US 9,232,337 B2](https://patents.google.com/patent/US9232337B2/en).

## Publication checklist

- Record the exact source commit and SHA-256 hashes for published binaries.
- Attach raw real-endpoint, latency, performance, and visual/DPI results.
- Run the v2 native and Python suites from the release commit.
- Independently run the frozen v1 suite.
- Confirm `LICENSE`, `NOTICE`, provenance, evaluation results, and these known
  limitations are included in the release archive.
