# Recognition training

Recognition packages preserve the serialized `package_version: 4` and `stereo-onset-v4` contract used by the C++ runtime.

Prepare the grouped asset manifest and generate the locked session corpus with the optional offline Steam Audio renderer:

```powershell
echoradar-ml prepare-assets --asset-root sounds --output ml\generated\recognition\asset-manifest.csv
echoradar-ml corpus `
  --manifest ml\generated\recognition\asset-manifest.csv `
  --asset-root sounds `
  --output ml\generated\recognition\sessions `
  --steam-audio-renderer build\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe
```

Cache, train, check export parity, and evaluate:

```powershell
echoradar-ml cache --sessions ml\generated\recognition\sessions --output ml\generated\recognition\feature-cache
echoradar-ml train --sessions ml\generated\recognition\sessions --cache ml\generated\recognition\feature-cache --output models\recognition-candidate
echoradar-ml parity --model models\recognition-candidate --fixture models\recognition-candidate\onnx_parity.npz
echoradar-ml evaluate --sessions ml\generated\recognition\sessions --model models\recognition-candidate --output ml\runs\recognition-candidate --require-gates
```

The wrapper at `ml/scripts/run_cpu_training.ps1` performs the same sequence. Steam Audio and all datasets are training-only. Do not move locked test sessions into training or change package metadata to make an incompatible model load.
