# EchoRadar v2 runtime

## Audio contract

The Windows runtime reads the endpoint's shared-mode mix format directly, then
captures it through WASAPI loopback as 48 kHz float PCM while preserving the
endpoint channel order and speaker roles. Miniaudio performs any client-side
resampling, but directional radar is enabled only when the independently
reported native mix format has an explicit, matching 5.1 or
7.1 `WAVEFORMATEXTENSIBLE` speaker mask. LFE is retained in diagnostics and
recordings but excluded from radar and recognition input.

The directional convention is shared by DSP, logs, dashboard, and HUD:

- 0 degrees: front
- 90 degrees: right
- 180 degrees: rear
- 270 degrees: left

5.1 uses front left/right at 330/30 degrees, center at 0, and surrounds at
250/110. 7.1 adds sides at 270/90 and backs at 210/150. `sampleRate` in capture
status is the 48 kHz processing rate; `nativeSampleRate` is the endpoint mix
rate. Resampling between 8–192 kHz and 48 kHz does not invalidate matching
channel roles. Stereo, missing roles, duplicate
roles, converted layouts, or malformed/missing native masks are reported
without synthesizing a directional layout.

The capture callback only copies to a bounded SPSC ring and updates per-channel
meters. Endpoint changes, layout changes, restart, overflow, and excessive
backlog advance the stream generation. Radar, recognition, history, and pending
events reset together at that boundary. A packet gap over 250 ms also resets
processing context and expires live event markers, retaining historical results.

## Radar

Each channel is windowed with a 2,048-sample Hann window and transformed every
480 samples. Per-bin channel power is combined with unit vectors for the
channel azimuths. The vector angle supplies bearing and its normalized length
supplies directivity. Weighted energy is linearly interpolated between adjacent
15-degree sectors, converted to dBFS, then passed through configurable attack,
release, and hold.

Continuous mode publishes every active sector and a strongest-sector arrow.
Event mode uses the unchanged stereo recognition model as a trigger and applies
the same multichannel radar to the corresponding history window. At most three
local energy peaks are returned; they must be within 18 dB of the strongest and
separated by at least 30 degrees. Combined mode displays both paths. A loaded
recognition model runs in every display mode; selecting Continuous no longer
disables recognition or its event history.

Opposing same-bin energy can cancel in the energy vector. Peaks are directional
energy maxima—not source separation—and only azimuth is reported.

## Headphone activity

Stereo playback remains stereo. An independent RMS detector uses -66/-72 dBFS
hysteresis and a 150 ms release hold. The headphone view shows left/right energy
balance; it does not claim front/rear azimuth or height. Capture, recognition,
and direction have separate status indicators.

## Recognition downmix

Recognition continues to consume the `stereo-onset-v4` interface. Front left
and right feed their respective outputs; center and surround/back channels are
attenuated and panned according to side; LFE is omitted. The downmix is bounded
to avoid clipping. Recognition and direction package formats are unchanged.

## UI/runtime boundary

The processing thread publishes immutable `AppSnapshot` values. The dashboard
and HUD read the latest snapshot and submit typed `UiCommand` values; they do
not mutate live DSP state. The runtime drains commands at safe points and
publishes the resulting state in the next snapshot.

Settings schema 4 lives at `%LOCALAPPDATA%\EchoRadar\v2\settings.json`.
Sessions live below `%LOCALAPPDATA%\EchoRadar\v2\sessions`. JSONL schema 3 uses
`stream_status`, `radar_frame`, and `event_direction` records. Continuous
frames are rate-limited to 10 Hz while active; event results are written once.
