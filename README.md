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
REM Capture from the system default input device (recommended)
audio_monitor

REM List all available input devices
audio_monitor --list-devices
audio_monitor -l

REM Show device types (microphone, stereo mix, virtual cable, etc.)
audio_monitor --list-devices-detailed

REM Capture from a named device (partial name match, case-insensitive)
audio_monitor --device "CABLE Output"
audio_monitor --device "Stereo Mix"
audio_monitor --device "Microphone"
audio_monitor -d "mic"
```

**Device Types:**
- 🎙️ **Microphone** (ambient sound) - your physical mic
- 🎮 **[GAME AUDIO]** (Stereo Mix) - Windows loopback device for internal audio
- 🔌 **[VIRTUAL CABLE]** (VB-Cable) - for advanced routing via OBS
- Other types shown with `--list-devices-detailed`

**Note:** The `-d` / `--device` flag REQUIRES a device name. Running just `audio_monitor -d` will return an error.

**Example output:**
```
[EchoRadar Audio Monitor]
Using device: Stereo Mix (system audio)

[AudioCapture] Started: Stereo Mix  ch=2  rate=48000 Hz
Press Ctrl+C to stop.

[   1] L RMS:  0.000  R RMS:  0.000  L Peak:  0.000  R Peak:  0.000  Buf:      0 fr
[   2] L RMS:  0.000  R RMS:  0.000  L Peak:  0.000  R Peak:  0.000  Buf:      0 fr
[   3] L RMS:  0.021  R RMS:  0.019  L Peak:  0.062  R Peak:  0.057  Buf:      0 fr
[   4] L RMS:  0.013  R RMS:  0.013  L Peak:  0.036  R Peak:  0.039  Buf:      0 fr
[   5] L RMS:  0.001  R RMS:  0.001  L Peak:  0.002  R Peak:  0.002  Buf:      0 fr
```

The output updates every 100 ms and continues until you press `Ctrl+C`.

---

## Setting Up Game Audio Capture

By default, EchoRadar captures from your **physical microphone** (ambient sound). However, for **CS2 gunshot detection**, you need to capture **game audio output** instead.

Windows provides several options:

### Option 1: Enable Windows Stereo Mix (Recommended if available)

**What:** Capture your PC's internal audio output as if it were a microphone input.

**Steps:**

1. **Check if Stereo Mix is available:**
   - Right-click the speaker icon in the Windows taskbar → **Open Volume mixer**
   - Click **Volume mixer** or **Advanced** > **App volume and device preferences**
   - Look for an input device called "Stereo Mix", "What U Hear", "Wave Out Mix", or similar

2. **If NOT visible, enable it:**
   - Right-click the speaker icon → **Open Sound settings**
   - Scroll down and click **Advanced** > **Volume mixer**
   - Click **Show inactive devices** (or **Show disabled devices**)
   - Right-click on "Stereo Mix" → **Enable**
   - If still not visible: right-click in the recording devices list → **Show disabled devices**
   - Right-click "Stereo Mix" → **Enable** and **Set as default**

3. **Test it:**
   ```bat
   .\build\tools\audio_monitor\Release\audio_monitor.exe --list-devices-detailed
   ```
   Look for a device with type **[system audio]**:
   ```
   [0] Stereo Mix (system audio)  [SYSTEM DEFAULT]
   ```

4. **Capture game audio:**
   ```bat
   .\build\tools\audio_monitor\Release\audio_monitor.exe
   ```
   Run CS2 or another game. You should see RMS/peak values respond to game audio (gunshots, footsteps).

**Troubleshooting:**
- If Stereo Mix doesn't appear: some audio drivers don't support it (e.g., Intel onboard, some USB devices)
- If not selectable: check Windows audio drivers in Device Manager; update if outdated
- No values appearing? Check Windows Volume mixer to ensure Stereo Mix is NOT muted

---

### Option 2: VB-Audio Virtual Cable (Works on most systems)

**What:** A virtual audio loopback device. Any audio can be routed to it and captured by EchoRadar.

**Setup:**

1. **Download and install VB-Cable:**
   - Visit: https://vb-audio.com/Cable/
   - Download the installer
   - Run as Administrator, reboot when prompted

2. **Verify installation:**
   ```bat
   .\build\tools\audio_monitor\Release\audio_monitor.exe --list-devices
   ```
   Look for `CABLE Output` or `VB-Audio Virtual Cable`:
   ```
   [1] CABLE Output (VB-Audio Virtual Cable)  [VIRTUAL CABLE]
   ```

3. **Route audio to VB-Cable** (see OBS section below if using OBS)

4. **Capture with EchoRadar:**
   ```bat
   .\build\tools\audio_monitor\Release\audio_monitor.exe --device "CABLE"
   ```

---

### Option 3: OBS Virtual Audio Output (Best for streaming + detection)

**What:** Use OBS to capture and route your game audio to a virtual cable for EchoRadar.

**Setup:**

1. **Install VB-Cable** (see Option 2 above)

2. **In OBS:**
   - Click **Settings** (bottom-right)
   - Go **Audio**
   - Under **Monitoring Devices**: select `VB-Cable Input` for the output you want to monitor
   - (Or: Right-click an audio source → **Audio Monitoring** → **Monitor and Output**)

3. **Alternatively, use OBS's built-in routing:**
   - In OBS, right-click your desktop audio source → **Filters**
   - Add a filter → **Audio Monitor** 
   - Set Output Device to `VB-Cable Input`

4. **Test with EchoRadar:**
   ```bat
   .\build\tools\audio_monitor\Release\audio_monitor.exe --device "CABLE"
   ```
   Launch CS2 in OBS (or your streaming setup). EchoRadar should now capture game audio.

---

### Quick Reference: Which Option to Use?

| Scenario | Recommendation |
|----------|---|
| Want to play CS2 locally (no streaming) | **Stereo Mix** (if available) or **VB-Cable** |
| Already streaming with OBS | **OBS Virtual Cable routing** |
| Stereo Mix unavailable | **VB-Cable** |
| Need perfect game audio fidelity | **VB-Cable** (no quality loss) |

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
  [0] Stereo Mix  <default>  [GAME AUDIO]
  [1] CABLE Output  [VIRTUAL CABLE]
  [2] Microphone (Realtek Audio)
```

**For detailed device type information, run:**
```
audio_monitor --list-devices-detailed
```
```
[EchoRadar] Available input devices (3):
  [0] Stereo Mix
       Type: system audio (Stereo Mix)  [SYSTEM DEFAULT]
  [1] CABLE Output (VB-Audio Virtual Cable)
       Type: virtual cable  [VIRTUAL CABLE]
  [2] Microphone (Realtek Audio)
       Type: microphone (ambient sound)
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
- **Buf**: Number of stereo frames currently buffered (0 for Milestone 1, since ring buffer is disabled)

### Known Limitations

**Milestone 1 (Current):** 
- Ring buffer (audio PCM storage) is temporarily disabled due to a buffer overflow issue being investigated
- This doesn't affect level monitoring (RMS/Peak), which works perfectly
- The `Buf` count will always show 0 because frames are not being stored
- For Milestone 3 (STFT analysis), the ring buffer will be fixed to enable full audio data access

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
