from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import subprocess
import tempfile

import numpy as np

from . import SAMPLE_RATE
from .audio import Audio, load_pcm_wav, to_stereo_48k, write_pcm16_wav


STEAM_AUDIO_VERSION = "v4.8.1"


@dataclass(frozen=True)
class SpatialParameters:
    azimuth_degrees: float
    elevation_degrees: float
    distance_meters: float
    occlusion: float
    transmission_low: float
    transmission_mid: float
    transmission_high: float
    directivity: float
    reverb_mix: float

    @property
    def azimuth_quadrant(self) -> str:
        angle = self.azimuth_degrees % 360.0
        if angle < 45.0 or angle >= 315.0:
            return "front"
        if angle < 135.0:
            return "right"
        if angle < 225.0:
            return "rear"
        return "left"


class SteamAudioRenderer:
    """Strict adapter for the pinned, offline-only Steam Audio renderer tool."""

    def __init__(self, executable: str | Path):
        self.executable = Path(executable)
        if not self.executable.is_file():
            raise FileNotFoundError(f"Steam Audio renderer is unavailable: {self.executable}")
        result = subprocess.run(
            [str(self.executable), "--version"], check=True, capture_output=True, text=True
        )
        expected = f"echoradar-steam-audio-renderer {STEAM_AUDIO_VERSION}"
        if result.stdout.strip() != expected:
            raise RuntimeError(
                f"Steam Audio renderer version mismatch: expected {expected!r}, got {result.stdout.strip()!r}"
            )

    def render(self, samples: np.ndarray, parameters: SpatialParameters) -> np.ndarray:
        mono = samples.astype(np.float32, copy=False).mean(axis=1, keepdims=True)
        with tempfile.TemporaryDirectory(prefix="echoradar-steam-audio-") as temporary:
            root = Path(temporary)
            source = root / "source.wav"
            output = root / "rendered.wav"
            write_pcm16_wav(source, Audio(mono, SAMPLE_RATE))
            subprocess.run([
                str(self.executable), "--input", str(source), "--output", str(output),
                "--azimuth", str(parameters.azimuth_degrees),
                "--elevation", str(parameters.elevation_degrees),
                "--distance", str(parameters.distance_meters),
                "--occlusion", str(parameters.occlusion),
                "--transmission", ",".join(str(value) for value in (
                    parameters.transmission_low, parameters.transmission_mid,
                    parameters.transmission_high,
                )),
                "--directivity", str(parameters.directivity),
                "--reverb-mix", str(parameters.reverb_mix),
            ], check=True)
            return to_stereo_48k(load_pcm_wav(output)).samples
