# Direction estimation and overlay

EchoRadar localizes each accepted recognition event independently. It copies a
bounded stereo window around that event's acoustic onset, extracts pooled
interaural level, delay, frequency-band level, and coherence cues, and produces
a 24-bin circular probability distribution. The HUD displays the primary mode
as an uncertainty arc; an optional second arc exposes a sufficiently strong,
well-separated front/rear candidate instead of presenting false precision.

The default policy localizes both footsteps and gunshots. The
recognition **Pulse width** remains a chart/event-display control. The separate
**Localization sample length** determines how much stereo audio contributes to
one direction estimate.

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
3. After the configured post-onset audio is available, the job copies its own
   window, saves that exact stereo window as a PCM16 WAV, and runs the estimator
   once when that class is enabled. The Direction page's **Play** button replays
   the saved file; clips are stored under `sessions/clips/<session-id>/` beside
   the settings file.
4. Low-confidence estimates are still drawn with reduced opacity so accepted
   recognizer events do not silently disappear; unavailable audio remains in
   diagnostics because it has no defensible bearing.
5. Estimated events are logged to `sessions/latest.jsonl` beside `settings.json`.

Discontinuities, default-device changes, and backlog resets clear both history
and pending jobs so windows never combine two capture generations.

## Calibration

Use a private/practice session with a single repeatable remote footstep source.
Keep the selected output device and CS2/Windows audio settings fixed.

- Quick mode: 8 bearings at 45-degree spacing, 3 accepted events each.
- Full mode: 24 bearings at 15-degree spacing, 4 accepted events each.
- Face the fixed source at the displayed relative bearing, arm capture, and
  produce one remote event. One event advances exactly one target.

Calibration profiles are stored atomically beside the settings file. The UI
shows whether the current profile matches or is stale.

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

The command renders one clip per source/bearing and writes
`direction-manifest.jsonl` with source hashes, labels, renderer version, and the
same pooled cue family used by the native baseline. Real-CS2 calibration remains
the preferred profile for a player's actual audio chain.
