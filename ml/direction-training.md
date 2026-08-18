# Direction training

The current direction model estimates 0–3 simultaneous gunshot/footstep sources from one fixed stereo scene. Synthetic Steam Audio scenes support training and initial gates; held-out real-CS2 validation remains mandatory.

Build the optional offline renderer with the pinned Steam Audio 4.8.1 SDK:

```powershell
cmake -S . -B build-renderer `
  -DECHORADAR_BUILD_APP=OFF `
  -DECHORADAR_BUILD_AUDIO_MONITOR=OFF `
  -DECHORADAR_BUILD_STEAM_AUDIO_RENDERER=ON `
  -DSTEAMAUDIO_ROOT="C:\path\to\steamaudio"
cmake --build build-renderer --config Release
```

Generate scenes, cache `[5,48,64]` features, train, and validate the package:

```powershell
echoradar-ml direction-mixtures `
  --manifest ml\generated\recognition\asset-manifest.csv `
  --asset-root sounds `
  --output ml\generated\direction `
  --steam-audio-renderer build-renderer\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe
echoradar-ml direction-cache --manifest ml\generated\direction\direction-scenes.jsonl --audio-root ml\generated\direction --output ml\generated\direction\feature-cache
echoradar-ml direction-train --cache ml\generated\direction\feature-cache --output models\direction-candidate
echoradar-ml direction-parity --model models\direction-candidate
echoradar-ml direction-evaluate --manifest ml\generated\direction\direction-scenes.jsonl --audio-root ml\generated\direction --model models\direction-candidate --split test --require-gates
```

The packaged preprocessing string remains `stereo-onset-v4-scene48`. Evaluation has no model-less mode. Follow [the real validation protocol](../docs/direction-validation.md) before promotion.
