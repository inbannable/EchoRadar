# Algorithm provenance and patent notice

EchoRadar v2 is an original implementation of a publicly described
multichannel energy-vector direction method. For each STFT frequency bin it
sums channel power and channel-direction vectors, derives an azimuth and
directivity from the resulting vector, then interpolates weighted energy onto
24 display sectors. Project code, visual assets, parameter choices, smoothing,
presets, UI, and HUD were developed for EchoRadar.

No ASUS source code, binaries, reverse-engineered parameters, graphical assets,
branding, or confidential material were used. The labels `All`, `Footsteps`,
and `Gunshots` are spectral-emphasis presets only. They must not be described as
semantic classifiers.

The method is not claimed to be bit-for-bit compatible with ASUS Sonic Radar.
Accuracy depends on the Windows channel mask, content mix, room simulation, and
endpoint processing. EchoRadar reports azimuthal energy peaks, not elevation or
proof that peaks correspond to distinct physical sources.

## Third-party patent rights

The Apache License, Version 2.0 grants patent rights from project contributors
for their contributions. It does not grant licenses to patents held by third
parties. Users and distributors must perform their own patent review. One
identified third-party patent is
[US 9,232,337 B2](https://patents.google.com/patent/US9232337B2/en). Mentioning it
is a disclosure, not a conclusion about validity, scope, infringement, or the
availability of a license.
