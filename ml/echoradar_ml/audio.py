from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import wave

import numpy as np

from . import SAMPLE_RATE


@dataclass(frozen=True)
class Audio:
    samples: np.ndarray  # float32 [frames, channels]
    sample_rate: int

    @property
    def channels(self) -> int:
        return int(self.samples.shape[1])


def load_pcm_wav(path: str | Path) -> Audio:
    with wave.open(str(path), "rb") as wav:
        if wav.getcomptype() != "NONE":
            raise ValueError(f"compressed WAV is unsupported: {path}")
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        frames = wav.getnframes()
        if channels not in (1, 2) or width not in (1, 2) or sample_rate <= 0:
            raise ValueError(f"expected PCM8/PCM16 mono/stereo WAV: {path}")
        raw = wav.readframes(frames)
    if width == 1:
        values = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    else:
        values = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    return Audio(values.reshape(-1, channels), sample_rate)


def write_pcm16_wav(path: str | Path, audio: Audio) -> None:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    values = np.clip(audio.samples, -1.0, 1.0)
    pcm = np.where(values < 0, values * 32768.0, values * 32767.0)
    pcm = np.rint(pcm).astype("<i2")
    with wave.open(str(output), "wb") as wav:
        wav.setnchannels(audio.channels)
        wav.setsampwidth(2)
        wav.setframerate(audio.sample_rate)
        wav.writeframes(pcm.tobytes())


def resample_linear(audio: Audio, target_rate: int = SAMPLE_RATE) -> Audio:
    if audio.sample_rate == target_rate:
        return Audio(np.array(audio.samples, dtype=np.float32, copy=True), target_rate)
    if target_rate <= 0 or len(audio.samples) == 0:
        raise ValueError("sample rates and audio length must be positive")
    output_frames = int(round(len(audio.samples) * target_rate / audio.sample_rate))
    positions = np.arange(output_frames, dtype=np.float64) * audio.sample_rate / target_rate
    left = np.minimum(positions.astype(np.int64), len(audio.samples) - 1)
    right = np.minimum(left + 1, len(audio.samples) - 1)
    fraction = (positions - left).astype(np.float32)[:, None]
    output = audio.samples[left] + (audio.samples[right] - audio.samples[left]) * fraction
    return Audio(output.astype(np.float32), target_rate)


def to_stereo_48k(audio: Audio) -> Audio:
    samples = audio.samples
    if audio.channels == 1:
        samples = np.repeat(samples, 2, axis=1)
    return resample_linear(Audio(samples, audio.sample_rate), SAMPLE_RATE)


def rms(samples: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(samples, dtype=np.float64)))) if samples.size else 0.0


def trim_activity(audio: Audio, before_seconds: float = 0.02, after_seconds: float = 0.20) -> Audio:
    envelope = np.max(np.abs(audio.samples), axis=1)
    if not len(envelope):
        return audio
    threshold = max(0.005, float(envelope.max()) * 0.02)
    active = np.flatnonzero(envelope >= threshold)
    if not len(active):
        return audio
    before = int(round(before_seconds * audio.sample_rate))
    after = int(round(after_seconds * audio.sample_rate))
    start = max(0, int(active[0]) - before)
    end = min(len(audio.samples), int(active[-1]) + after + 1)
    return Audio(audio.samples[start:end].copy(), audio.sample_rate)
