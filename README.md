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
| 1 | AudioCapture | 🔲 |
| 2 | RingBuffer | 🔲 |
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
