# EchoRadar v2

EchoRadar v2 is Windows research software that captures headphone playback
audio and turns discrete 5.1/7.1 system-output audio into a 24-sector azimuth radar. It uses an original
frequency-domain multichannel energy-vector implementation and a tactical-dark
dashboard/HUD. It does not use ASUS code, assets, branding, or proprietary
tuning, and it does not claim bit-for-bit compatibility with ASUS Sonic Radar.

The playback signal path preserves native channel roles:

```text
Windows WASAPI loopback (stereo or WAVEFORMATEXTENSIBLE 5.1/7.1)
  -> 48 kHz analysis PCM, retaining the endpoint channel map
  +-> independent audio activity / headphone left-right energy monitor
  +-> 2048-sample Hann STFT / 480-sample hop
  |    -> per-bin multichannel energy vector
  |    -> 24 azimuth sectors + smoothing -> dashboard and HUD
  +-> LFE-free stereo downmix -> unchanged stereo-onset-v4 recognizer
       -> optional event-window radar peaks
```

Stereo headphones support capture, activity monitoring, and optional event
recognition. Full azimuth requires discrete 5.1/7.1 channels; sample-rate
conversion is allowed when the channel-role map is preserved. EchoRadar v2 never invents surround bearings
from stereo. The frozen v1 implementation remains at branch
`codex/v1-maintenance` and annotated tag `echoradar-v1.0.0`.

## Features

- Native Windows 5.1 and 7.1 channel-role preservation; LFE is excluded from
  directional analysis and recognition downmix.
- Continuous, event, and combined radar modes.
- `All`, `Footsteps`, `Gunshots`, and editable 16-point `Custom` spectral
  emphasis curves. Preset names describe frequency emphasis only; they do not
  classify sound semantics.
- 24 sectors at 15-degree spacing, with 0 degrees front, 90 degrees right,
  180 degrees rear, and 270 degrees left.
- Resumable surround setup, actionable inline runtime states, a single
  keyboard-navigable dashboard, and a click-through/editable per-display HUD.
- Settings schema 4 under `%LOCALAPPDATA%\EchoRadar\v2` and JSONL schema 3
  session records.
- Optional existing `stereo-onset-v4` recognition packages run in every radar
  display mode. Audio activity and continuous surround radar need no model.

## Build

Windows 10/11 x64, CMake 3.20+, a C++20 toolchain, and DirectX 11 are required
for the application. ONNX Runtime CPU is required only when recognition is
enabled. The first configure fetches pinned KissFFT, miniaudio, ImGui, and—when
tests are enabled—GoogleTest.

```powershell
cmake -S . -B build `
  -DECHORADAR_BUILD_APP=ON `
  -DECHORADAR_BUILD_AUDIO_MONITOR=ON `
  -DECHORADAR_BUILD_TESTS=ON `
  -DECHORADAR_ENABLE_ONNX=ON `
  -DONNXRUNTIME_ROOT="C:\path\to\Microsoft.ML.OnnxRuntime"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The application target is `EchoRadarV2.exe`.

## Run

```powershell
.\build\src\app\Release\EchoRadarV2.exe `
  --radar-mode continuous `
  --radar-preset all
```

Recognition is optional:

```powershell
.\build\src\app\Release\EchoRadarV2.exe `
  --model models\recognition-candidate `
  --radar-mode combined `
  --radar-preset footsteps
```

Use `--list-audio-outputs` to print native formats and ordered channel roles,
`--audio-output-id <id>` to pin an endpoint, `--settings <json>` to override
the v2 settings file, and `--no-overlay` for a headless run.

## Accuracy and limitations

EchoRadar reports azimuth only. Sector peaks are energy maxima, not proof of
separate physical sources. Opposing sounds with energy in the same frequency
bins can cancel in an energy vector. Room reflections, game mixing, endpoint
processing, virtual-surround drivers, and incorrect Windows speaker masks can
reduce accuracy. Discrete 5.1/7.1 channel roles are required for full azimuth. Stereo energy
balance cannot distinguish front from rear.

The automated suite covers channel mapping, buffering/downmix, radar bearings,
interpolation, presets, smoothing, cancellation, state transfer, settings, and
HUD bounds. Real endpoint and visual/DPI results are tracked as release gates
in [docs/evaluation.md](docs/evaluation.md); unmeasured gates are not reported
as passes.

Draft release notes and the publication checklist are in
[docs/release-v2.0.0-research.md](docs/release-v2.0.0-research.md).

## License and patent notice

EchoRadar v2 is licensed under the [Apache License 2.0](LICENSE). Apache-2.0
grants patent rights from contributors only; it does not grant rights under
third-party patents. See [NOTICE](NOTICE) and
[algorithm provenance](docs/algorithm-provenance.md), including the notice for
[US 9,232,337 B2](https://patents.google.com/patent/US9232337B2/en).

Local `sounds/`, `models/`, `recordings/`, generated corpora, runs, caches, and
checkpoints are ignored and must not be redistributed without appropriate
rights.

The headphone investigation, implemented changes, remaining full-spatial work,
and real-device checklist are in [audio/headphone refactor](docs/audio-headphone-refactor.md).
