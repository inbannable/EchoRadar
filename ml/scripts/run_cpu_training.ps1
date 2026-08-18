param(
    [Parameter(Mandatory = $true)][string]$AssetRoot,
    [Parameter(Mandatory = $true)][string]$SteamAudioRenderer,
    [string]$WorkRoot = "ml\generated\recognition",
    [string]$ModelOutput = "models\recognition-candidate",
    [int]$Epochs = 20,
    [int]$BatchSize = 64,
    [int]$Threads = 0
)

$ErrorActionPreference = "Stop"
$Python = "ml\.venv\Scripts\python.exe"
if (-not (Test-Path $Python)) {
    throw "Missing ml virtual environment. Follow ml\recognition-training.md first."
}
if (-not (Test-Path $SteamAudioRenderer)) {
    throw "Steam Audio renderer not found: $SteamAudioRenderer"
}

$Manifest = Join-Path $WorkRoot "asset-manifest.csv"
$Sessions = Join-Path $WorkRoot "sessions"
$Cache = Join-Path $WorkRoot "feature-cache"
$ParityFixture = Join-Path $ModelOutput "onnx_parity.npz"
$EvaluationOutput = Join-Path "ml\runs" "recognition-candidate"

& $Python -m echoradar_ml.cli prepare-assets `
    --asset-root $AssetRoot --output $Manifest
& $Python -m echoradar_ml.cli corpus `
    --manifest $Manifest --asset-root $AssetRoot --output $Sessions `
    --steam-audio-renderer $SteamAudioRenderer
& $Python -m echoradar_ml.cli audit --sessions $Sessions
& $Python -m echoradar_ml.cli cache --sessions $Sessions --output $Cache
& $Python -m echoradar_ml.cli train `
    --sessions $Sessions --cache $Cache --output $ModelOutput `
    --epochs $Epochs --batch-size $BatchSize --threads $Threads
& $Python -m echoradar_ml.cli parity `
    --model $ModelOutput --fixture $ParityFixture
& $Python -m echoradar_ml.cli evaluate `
    --sessions $Sessions --model $ModelOutput --output $EvaluationOutput
