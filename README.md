# EchoRadar

Real-Time Spatial Audio Detection System for FPS Games

## Features

- Footstep Detection
- Gunshot Detection
- 360° Direction Estimation
- Overlay Rendering

## Requirements

| Tool | Version |
|------|---------|
| CMake | ≥ 3.20 |
| MSVC / Clang | C++20 |
| Windows SDK | 10.0+ (for DirectX 11 overlay) |

External libraries are fetched automatically via CMake `FetchContent`:

- **KissFFT** – FFT engine for STFT
- **miniaudio** – cross-platform audio capture (header-only)
- **Dear ImGui** – overlay UI (Windows + DirectX 11)
- **GoogleTest** – unit tests

## Build

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile
cmake --build build --config Release

# Run tests
ctest --test-dir build --output-on-failure
```

## Audio Monitor (Milestone 1)

`audio_monitor` captures stereo audio and prints live RMS / peak levels.
Use it to verify the capture pipeline before any DSP work.

```bat
:: List all input devices
.\build\tools\audio_monitor\Release\audio_monitor.exe --list-devices

:: Capture from VB-Cable / OBS virtual device
.\build\tools\audio_monitor\Release\audio_monitor.exe --device "CABLE Output"

:: Capture from the system default input
.\build\tools\audio_monitor\Release\audio_monitor.exe
```

Example output:
```
[EchoRadar Audio Monitor]
Using device: CABLE Output (VB-Audio Virtual Cable)

L RMS:  0.142  R RMS:  0.137  L Peak:  0.482  R Peak:  0.501  Buf:   4096 fr  [##########----------]
```

**Verification steps with OBS / VB-Cable:**
1. Route OBS desktop audio output → VB-Audio Virtual Cable input.
2. Run `audio_monitor --device "CABLE Output"`.
3. Play a game or YouTube video — RMS values should rise above ~0.05.
4. Mute the source — RMS values should fall back to ~0.000.

## Run

```
EchoRadar.exe
```

## Architecture

```
OBS Audio Capture
      │
      ▼
AudioCapture (miniaudio)
      │
      ▼
RingBuffer (lock-free, thread-safe)
      │
      ▼
STFTProcessor (KissFFT, 1024-pt, Hann window)
      │
      ├── GunshotDetector  ── GunshotEvent  ─┐
      │                                       ├─▶ FeatureExtractor
      └── FootstepDetector ── FootstepEvent  ─┘        │
                                                        ▼
                                           KNNDirectionEstimator
                                                        │
                                                        ▼
                                              DirectionTracker (Kalman)
                                                        │
                                                        ▼
                                            OverlayRenderer (ImGui/DX11)
```

## Milestone Roadmap

| # | Module | Status |
|---|--------|--------|
| 0 | Project Initialization | ✅ |
| 1 | AudioCapture (miniaudio, AudioDeviceManager, AudioRingBuffer) | ✅ |
| 2 | RingBuffer (DSP frame buffer) | ✅ |
| 3 | STFTProcessor | 🔲 |
| 4 | GunshotDetector | 🔲 |
| 5 | FootstepDetector | 🔲 |
| 6 | Dataset Recorder Tool | 🔲 |
| 7 | FeatureExtractor | 🔲 |
| 8 | KNNDirectionEstimator | 🔲 |
| 9 | DirectionTracker | 🔲 |
| 10 | OverlayRenderer | 🔲 |
| 11 | System Integration | 🔲 |
| 12 | Performance Optimization | 🔲 |

## License

MIT
