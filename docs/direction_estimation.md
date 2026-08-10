# Direction estimation and overlay

EchoRadar localizes each accepted recognition event independently. It searches
the broad recognition interval for the strongest smoothed stereo-RMS peak and
then analyzes only a class-specific short window around that peak. Footsteps
default to 18 ms before / 150 ms after the peak; gunshots default to 8 ms before
/ 75 ms after it. A single peak-derived activity mask weights broadband,
GCC-PHAT, and spectral cues. The v2 feature schema contains peak/noise quality,
GCC sharpness and peak-to-sidelobe quality, 24 logarithmic ILD/coherence bands,
and normalized left/right spectral shapes. These feed a versioned deterministic
mapper that produces a 24-bin circular probability distribution. The HUD displays the primary mode
as an uncertainty arc; an optional second arc exposes a sufficiently strong,
well-separated front/rear candidate instead of presenting false precision.

The default policy localizes both footsteps and gunshots. The
recognition **Pulse width** remains a chart/event-display control. The separate
**Localization sample length** remains the backward-compatible peak-search
window; class-specific peak settings are authoritative for feature extraction.

## Why the estimator is profile-conditioned

Steam Audio describes an HRTF as a pair of filters that changes amplitude,
arrival time, and spectral content at the two ears for a direction around the
listener. Its binaural API consumes a unit vector from listener to source and
can expose both ear peak delays. This is a forward rendering model, not a
closed-form inverse from an arbitrary mixed stereo recording back to one exact
bearing:

- [Steam Audio programmer's guide: HRTFs](https://valvesoftware.github.io/steam-audio/doc/capi/guide.html#hrtf)
- [Steam Audio binaural effect](https://valvesoftware.github.io/steam-audio/doc/capi/binaural-effect.html)
- [Steam Audio relative-direction geometry](https://valvesoftware.github.io/steam-audio/doc/capi/geometry.html)

CS2 also exposes EQ, L/R isolation, and perspective-correction controls. Source
2 describes perspective correction as spatializing from a source's on-screen
position instead of only its world-space position ([CS2 Source 2 schema
entry](https://s2v.app/SchemaExplorer/cs2/sounddoc_lib/CMixSteamAudioSource)).
Therefore EchoRadar records
these values in the audio-profile key and treats a saved real-CS2 calibration
as stale when any of them changes. Aspect ratio is recorded because the screen
projection is part of that correction. Windows/headset spatial enhancement and
the output endpoint are recorded for the same reason.

The synthetic baseline uses Steam Audio v4.8.1 known-bearing renders and returns
wide arcs. It is intended for immediate basic use. Guided calibration learns
feature prototypes from the player's real CS2 output chain and can resolve
profile-specific front/rear behavior.

## Runtime flow

1. WASAPI loopback writes sequential 48 kHz stereo frames to the recognizer and
   a three-second history buffer.
2. Every accepted event creates exactly one pending audio/localization job.
3. After the event and configured peak tail are available, the job copies its
   search window, selects the peak window, saves that exact selected stereo
   window as a PCM16 WAV, and runs the estimator
   once when that class is enabled. The Direction page's **Play** button replays
   the saved file; clips are stored under `sessions/clips/<session-id>/` beside
   the settings file.
4. Low-confidence estimates are still drawn with reduced opacity so accepted
   recognizer events do not silently disappear; unavailable audio remains in
   diagnostics because it has no defensible bearing.
5. Estimated events are logged to `sessions/latest.jsonl` beside `settings.json`.
   Logs include the peak sample, selected bounds, peak/noise and active-frame
   quality, GCC quality, feature schema, and mapper version.

Discontinuities, default-device changes, and backlog resets clear both history
and pending jobs so windows never combine two capture generations.

## Calibration

Use a private/practice session with a single repeatable remote footstep source.
Keep the selected output device and CS2/Windows audio settings fixed.

- Quick mode: 8 bearings at 45-degree spacing, 3 accepted events each.
- Full mode: 24 bearings at 15-degree spacing, 4 accepted events each.
- Face the fixed source at the displayed relative bearing, arm capture, and
  produce one remote event. One event advances exactly one target.

Calibration profiles are stored atomically beside the settings file. V2 uses
per-class median/MAD normalization, quality-gated samples, and a bounded
circular nearest-neighbor adaptation. Eight-band v1 profiles are rejected with
a recalibration message. The UI shows whether the current profile matches or is stale.

## HUD behavior

The HUD is a separate topmost, non-activating, click-through Windows window with three modes:
off, CS2 foreground only, or always visible. It follows the CS2 client rectangle
and exposes radius, thickness, opacity, offsets, class lifetimes, center dot,
and secondary-candidate controls. The HUD never owns or changes the mouse
cursor, so CS2 keeps the cursor hidden during borderless-fullscreen play and
all clicks continue to reach the game. `Ctrl+Alt+O` hides or restores it globally.

Use CS2 Fullscreen Windowed/Borderless. Exclusive fullscreen may bypass normal
desktop composition. Microsoft documents that composition targets and layered
content belong to the app's own window, which is the architecture used here:
[DirectComposition window targets](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondesktopdevice-createtargetforhwnd).

## Synthetic known-bearing corpus

After building the pinned Steam Audio renderer:

```powershell
echoradar-ml direction-corpus `
  --source-root sounds `
  --output ml\generated\direction `
  --steam-audio-renderer build\tools\steam_audio_renderer\Release\echoradar_steam_audio_renderer.exe `
  --azimuth-step 15
```

By default the CLI renders clean, gain, noise, mild-reverb, occlusion, and
channel-isolation conditions for every source/bearing and writes
`direction-manifest.jsonl` with source hashes, labels, ground-truth peak timing,
selected bounds, renderer version, and the same v2 cues used by native code.
The source hash deterministically assigns every recording (and all of its
rendered variants) to `train` or `validation`, preventing source-identity
leakage. Use `--split validation` for the held-out gate.
Evaluate the manifest and save the success-gate report with:

```powershell
echoradar-ml direction-evaluate `
  --manifest ml\generated\direction\direction-manifest.jsonl `
  --split validation `
  --error-clips ml\generated\direction\errors `
  --output ml\generated\direction\evaluation.json
```

The report includes median/P90/maximum circular error, left/right and
front/rear accuracy, high-confidence catastrophic rate, mean confidence, and
per-class/per-angle breakdowns. Real-CS2 calibration remains
the preferred profile for a player's actual audio chain.
