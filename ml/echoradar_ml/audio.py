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


def resample_bandlimited(audio: Audio, target_rate: int = SAMPLE_RATE) -> Audio:
    """Resample PCM with an FFT band-limited interpolator.

    Extracted CS2 assets are short, predominantly 44.1 kHz clips.  FFT
    resampling is deterministic, removes the imaging produced by the previous
    linear interpolator, and keeps the base package NumPy-only.  Runtime capture
    is already 48 kHz and therefore takes the copy-only fast path.
    """
    if audio.sample_rate == target_rate:
        return Audio(np.array(audio.samples, dtype=np.float32, copy=True), target_rate)
    if target_rate <= 0 or len(audio.samples) == 0:
        raise ValueError("sample rates and audio length must be positive")
    output_frames = int(round(len(audio.samples) * target_rate / audio.sample_rate))
    # Guard samples prevent the FFT's periodic boundary from wrapping a clip's
    # decay into its beginning, which would corrupt measured onset offsets.
    guard = min(1024, max(32, len(audio.samples) // 4))
    source = np.pad(audio.samples.astype(np.float64, copy=False), ((guard, guard), (0, 0)))
    input_frames = len(source)
    padded_output_frames = int(round(input_frames * target_rate / audio.sample_rate))
    spectrum = np.fft.rfft(source, axis=0)
    output_spectrum = np.zeros((padded_output_frames // 2 + 1, audio.channels), dtype=np.complex128)
    copied = min(len(spectrum), len(output_spectrum))
    output_spectrum[:copied] = spectrum[:copied]

    # A real-valued Nyquist bin represents both positive and negative
    # frequencies.  Split/merge it when the shorter transform has even length.
    shorter = min(input_frames, padded_output_frames)
    if shorter % 2 == 0 and copied > shorter // 2:
        nyquist = shorter // 2
        if padded_output_frames > input_frames:
            output_spectrum[nyquist] *= 0.5
        elif padded_output_frames < input_frames:
            output_spectrum[nyquist] *= 2.0
    output = np.fft.irfft(output_spectrum, n=padded_output_frames, axis=0)
    output *= padded_output_frames / input_frames
    trim_start = int(round(guard * target_rate / audio.sample_rate))
    output = output[trim_start:trim_start + output_frames]
    return Audio(output.astype(np.float32), target_rate)


# Source compatibility for callers outside the ML package.  The implementation
# is intentionally no longer linear.
resample_linear = resample_bandlimited


def to_stereo_48k(audio: Audio) -> Audio:
    samples = audio.samples
    if audio.channels == 1:
        samples = np.repeat(samples, 2, axis=1)
    return resample_bandlimited(Audio(samples, audio.sample_rate), SAMPLE_RATE)


def rms(samples: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(samples, dtype=np.float64)))) if samples.size else 0.0


def activity_bounds(audio: Audio, before_seconds: float = 0.02,
                    after_seconds: float = 0.20) -> tuple[int, int, int]:
    """Return ``(trim_start, trim_end, audible_onset)`` in input samples."""
    envelope = np.max(np.abs(audio.samples), axis=1)
    if not len(envelope):
        return 0, 0, 0
    threshold = max(0.005, float(envelope.max()) * 0.02)
    active = np.flatnonzero(envelope >= threshold)
    if not len(active):
        return 0, len(audio.samples), 0
    before = int(round(before_seconds * audio.sample_rate))
    after = int(round(after_seconds * audio.sample_rate))
    start = max(0, int(active[0]) - before)
    end = min(len(audio.samples), int(active[-1]) + after + 1)
    return start, end, int(active[0])


def trim_activity_with_offset(
    audio: Audio, before_seconds: float = 0.02, after_seconds: float = 0.20,
) -> tuple[Audio, int]:
    """Trim silence while preserving the exact audible-onset offset.

    The old corpus generator retained 20 ms of pre-roll and then labeled the
    beginning of that pre-roll as the event onset.  Returning the offset makes
    that impossible for new callers.
    """
    start, end, audible = activity_bounds(audio, before_seconds, after_seconds)
    return Audio(audio.samples[start:end].copy(), audio.sample_rate), max(0, audible - start)


def trim_activity(audio: Audio, before_seconds: float = 0.02, after_seconds: float = 0.20) -> Audio:
    return trim_activity_with_offset(audio, before_seconds, after_seconds)[0]
