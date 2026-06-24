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

**Executable path:**
```
.\build\tools\audio_monitor\Release\audio_monitor.exe
```

**Basic usage:**
```bat
:: List all input devices
audio_monitor --list-devices
audio_monitor -l

:: Capture from a named device (partial name match, case-insensitive)
audio_monitor --device "CABLE Output"
audio_monitor -d "mic"

:: Capture from the system default input device
audio_monitor
```

**Example output:**
```
[EchoRadar Audio Monitor]
Using device: CABLE Output (VB-Audio Virtual Cable)

[AudioCapture] Started: CABLE Output (VB-Audio Virtual Cable)  ch=2  rate=48000 Hz
Press Ctrl+C to stop.

L RMS:  0.142  R RMS:  0.137  L Peak:  0.482  R Peak:  0.501  Buf:   4096 fr  [##########----------]
L RMS:  0.155  R RMS:  0.148  L Peak:  0.510  R Peak:  0.498  Buf:   4224 fr  [##########----------]
L RMS:  0.138  R RMS:  0.141  L Peak:  0.475  R Peak:  0.512  Buf:   4096 fr  [##########----------]
```

The output updates every 100 ms. Press `Ctrl+C` to stop.

---

## Milestone 1 Validation Guide

### Step 1: List Available Devices

Run:
```bat
.\build\tools\audio_monitor\Release\audio_monitor.exe --list-devices
```

Output should show all audio input devices on your system, with `<default>` marking the system default:
```
[EchoRadar] Available input devices (3):
  [0] Microphone (Realtek Audio)  <default>
  [1] CABLE Output (VB-Audio Virtual Cable)
  [2] Line In (Line Input)
```

**What to look for:**
- At least one input device must be present
- If no devices appear, verify audio drivers are installed and working in Windows Settings

### Step 2: Test with System Default Device

Run:
```bat
.\build\tools\audio_monitor\Release\audio_monitor.exe
```

Watch the output for 10–15 seconds:

**Silence test (no audio input):**
```
L RMS:  0.000  R RMS:  0.000  L Peak:  0.000  R Peak:  0.000  Buf:    256 fr  [----------]
```
- All RMS and peak values should be near 0.000
- Buffer frame count should remain stable (~256–2048 frames depending on timing)
- Bar chart should be empty `----------`

**With audio (speak, play music, etc.):**
```
L RMS:  0.087  R RMS:  0.092  L Peak:  0.267  R Peak:  0.289  Buf:    512 fr  [###-------]
```
- RMS values should rise to 0.05–0.30 depending on input volume
- Peak values should be noticeably higher than RMS
- Bar chart should show visual feedback proportional to left channel RMS
- Buffer frame count may fluctuate slightly but should stay under ~4096 frames

### Step 3: Test Stereo Left/Right Channel Separation

If your device has stereo input (e.g., CABLE Output routing from OBS):

**Setup:**
1. Route audio to left channel only
2. Run: `audio_monitor`
3. Observe that **only L RMS and L Peak are non-zero**:
   ```
   L RMS:  0.150  R RMS:  0.000  L Peak:  0.450  R Peak:  0.000
   ```

4. Route audio to right channel only
5. Observe that **only R RMS and R Peak are non-zero**:
   ```
   L RMS:  0.000  R RMS:  0.155  L Peak:  0.000  R Peak:  0.460
   ```

If both channels are equal (stereo mix):
```
L RMS:  0.145  R RMS:  0.148  L Peak:  0.420  R Peak:  0.425
```

**Why this matters:**
- Confirms stereo channel separation is working
- Ensures left/right calibration won't be confused during direction estimation (Milestone 8)

### Step 4: Test with OBS Virtual Cable Setup (Optional)

If you're using OBS + VB-Cable for game audio capture:

**Setup:**
1. In OBS, route desktop audio to VB-Audio Virtual Cable
2. Run: `audio_monitor --device "CABLE"`
3. Play a game or YouTube video
4. Expected output:
   ```
   L RMS:  0.100  R RMS:  0.105  L Peak:  0.340  R Peak:  0.350  Buf:    768 fr  [#####-----]
   ```

**Troubleshooting:**
- If device not found: run `--list-devices` to see exact name
- If RMS stays at 0.0: check OBS routing in Volume Mixer
- If buffer count grows continuously: audio source is too fast; this is OK for Milestone 1

### Typical RMS/Peak Ranges

| Scenario | L RMS | R RMS | L Peak | R Peak |
|----------|-------|-------|--------|--------|
| Complete silence | 0.000 | 0.000 | 0.000 | 0.000 |
| Quiet speech | 0.02–0.05 | 0.02–0.05 | 0.10–0.20 |  0.10–0.20 |
| Normal speech | 0.05–0.15 | 0.05–0.15 | 0.25–0.50 | 0.25–0.50 |
| Loud speech / music | 0.15–0.40 | 0.15–0.40 | 0.50–0.95 | 0.50–0.95 |
| Game gunshot | 0.30–0.50 | 0.30–0.50 | 0.80–0.99 | 0.80–0.99 |

**Note:** These are guidelines; actual values depend on mic sensitivity, input volume, and audio content.

### What Each Field Means

- **L RMS / R RMS**: Root Mean Square (volume) of left/right channel in the last 100 ms. Range: [0.0, 1.0]
- **L Peak / R Peak**: Highest absolute sample value in left/right channel in the last 100 ms. Range: [0.0, 1.0]
- **Buf**: Number of stereo frames currently buffered in the ring buffer (capacity: ~192,000 @ 48 kHz stereo)
- **[###---]**: Visual bar chart of left channel RMS for quick feedback

### Verification Checklist

- [ ] `--list-devices` shows at least one input device
- [ ] Default device capture shows 0.000 RMS in silence
- [ ] Speaking / playing audio causes RMS to rise above 0.05
- [ ] Peak values are consistently ≥ RMS values
- [ ] Buffer count stays bounded (not continuously growing)
- [ ] Stereo channels (if available) show independent L/R values
- [ ] No crashes or error messages during capture
- [ ] Clean shutdown with `Ctrl+C`

### Safeguards Built In

- **Lock-free ring buffer** (AudioRingBuffer): Single-producer (audio callback) / single-consumer (main thread) design ensures no mutex overhead or deadlocks
- **Atomic level updates** (AudioCapture::Impl::leftRms, rightRms, etc.): Thread-safe without locks
- **Overflow handling**: If capture outpaces consumption, new frames are silently dropped (never blocks the audio thread)
- **Format validation**: After opening device, verify 2-channel 48 kHz output; if conversion is needed, a warning is logged
- **Proper cleanup**: Signals (SIGINT, SIGTERM) trigger graceful shutdown with device uninit

If you encounter issues:
1. Check Windows audio settings
2. Verify audio drivers
3. Use `--list-devices` to confirm device name
4. Try `--device "default"` or partial name match
5. Check system volume is not muted

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
