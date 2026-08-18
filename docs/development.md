# Development

## Supported configurations

The application and audio monitor build only on Windows 10/11 x64. Non-Windows hosts build the supported libraries and can run the C++ and Python regression suites. CMake 3.20+ and a C++20 compiler are required. The first configure needs network access for pinned kissfft and miniaudio sources, plus ImGui on Windows application builds.

The main options are:

- `ECHORADAR_BUILD_APP=ON`: build `EchoRadar` on Windows.
- `ECHORADAR_BUILD_AUDIO_MONITOR=ON`: build the Windows loopback monitor.
- `ECHORADAR_ENABLE_ONNX=ON`: enable both recognition and direction inference.
- `ECHORADAR_BUILD_TESTS=ON`: fetch GoogleTest and build retained tests.
- `ECHORADAR_BUILD_STEAM_AUDIO_RENDERER=ON`: build the optional offline training renderer.

## Library-only build

```powershell
cmake -S . -B build -DECHORADAR_BUILD_APP=OFF -DECHORADAR_BUILD_AUDIO_MONITOR=OFF
cmake --build build --config Release
```

This configuration does not fetch GoogleTest. Enable tests explicitly:

```powershell
cmake -S . -B build-tests `
  -DECHORADAR_BUILD_APP=OFF `
  -DECHORADAR_BUILD_AUDIO_MONITOR=OFF `
  -DECHORADAR_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

The retained native suite covers the SPSC audio ring, history buffer, streaming STFT, PCM WAV I/O, recognition package/features/peak policy, direction package/post-processing/scene grouping, and schema-2 settings migration.

## Source layout

```text
src/app          Windows application orchestration
src/audio        loopback capture, buffers, devices, PCM WAV
src/dsp          streaming STFT
src/recognition  current package, features, ONNX engine, recognizer
src/direction    multi-source package, ONNX engine, scene coordinator
src/settings     schema-3 runtime settings
src/overlay      control UI and click-through HUD
src/support      flat JSON and SHA-256 utilities
tools            loopback monitor and optional offline renderer
tests            active native regression suite
ml               active offline training and evaluation pipeline
```

Do not change serialized tensor names, shapes, `package_version: 4`, or `stereo-onset-v4` while making source-level refactors. Endpoint discontinuities must reset recognition, audio history, and pending direction scenes together.
