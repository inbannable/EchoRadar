# EchoRadar v2 release evaluation

The automated suite is necessary but not sufficient for a research release.
Results below must be recorded from the exact release commit and must not be
marked complete from simulation alone.

## Current working-tree evidence

These checks were last recorded locally on 2026-08-24. They show implementation
health only: the working tree is not a tagged release candidate, and these
results do not close any real-device or visual publication gate.

| Check | Result |
|---|---|
| Native Debug suite (`ctest --test-dir build -C Debug --output-on-failure`) | 85/85 passed |
| Native Release suite (`ctest --test-dir build -C Release --output-on-failure`) | 85/85 passed |
| Python suite (`ml\.venv\Scripts\python.exe -m pytest ml\tests -q`) | 31/31 passed |
| Release CLI/device smoke | `--help`, `--list-audio-outputs`, and the audio-monitor device listing exited successfully; a 0.5-second default-endpoint loopback run reached `Running` with generation 1 and no dropped/discarded frames. |
| Local stereo/UI smoke | A 2-channel/48 kHz endpoint was correctly reported as unsupported for directional radar. The current working tree rendered the dashboard and CJK endpoint name correctly, suppressed the HUD over the focused dashboard, displayed the HUD after focus moved away, and exited cleanly when the dashboard closed. Repeat on the exact release commit. |

## Quantitative gates

| Gate | Target | Result |
|---|---:|---|
| Median bearing error | <= 15 degrees | Not measured on real endpoints |
| 90th-percentile bearing error | <= 30 degrees | Not measured on real endpoints |
| High-confidence errors over 90 degrees | 0 | Not measured on real endpoints |
| Continuous display latency | < 50 ms | Not measured end-to-end |
| Event result latency | < 400 ms | Not measured end-to-end |
| DSP p95 per 10 ms hop | < 5 ms | Not measured on release hardware |
| Capture-callback allocations | 0 | Not measured with allocation instrumentation |

## Required matrix

- Real Windows 5.1 and 7.1 render endpoints at 48 kHz.
- Cardinal, intermediate, mirrored, simultaneous different-band, and opposing
  same-band scenes.
- Dashboard and HUD at 100%, 125%, 150%, and 200% DPI on 720p, 1080p, 1440p,
  and 4K displays.
- Keyboard navigation, contrast/shape cues, clipping, empty/silent states, and
  long endpoint/model names.
- Endpoint removal/recovery, default-device changes, stream discontinuities,
  malformed/missing recognition packages, and unsupported stereo/44.1 kHz.

The real 5.1/7.1 endpoint and bearing matrix, end-to-end latency and DSP
performance, capture-callback allocation behavior, and complete display/DPI
matrix remain unmeasured.

Record the OS build, endpoint/driver, speaker mask, display scale, test corpus
revision, executable SHA-256, raw measurements, and summary statistics with
every published result.
