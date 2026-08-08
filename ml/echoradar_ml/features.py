from __future__ import annotations

import numpy as np

from . import (
    FFT_SIZE, HOP_SIZE, MEL_BINS, PCEN_ALPHA, PCEN_DELTA, PCEN_EPSILON,
    PCEN_ROOT, PCEN_SMOOTHING, SAMPLE_RATE,
)


def _hz_to_mel(hz: np.ndarray | float) -> np.ndarray:
    return 2595.0 * np.log10(1.0 + np.asarray(hz) / 700.0)


def _mel_to_hz(mel: np.ndarray | float) -> np.ndarray:
    return 700.0 * (np.power(10.0, np.asarray(mel) / 2595.0) - 1.0)


def mel_filterbank(
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = FFT_SIZE,
    mel_bins: int = MEL_BINS,
    min_hz: float = 50.0,
    max_hz: float = 18_000.0,
) -> np.ndarray:
    points = _mel_to_hz(np.linspace(_hz_to_mel(min_hz), _hz_to_mel(max_hz), mel_bins + 2))
    frequencies = np.arange(fft_size // 2 + 1, dtype=np.float64) * sample_rate / fft_size
    filters = np.zeros((mel_bins, len(frequencies)), dtype=np.float32)
    for index in range(mel_bins):
        left, center, right = points[index : index + 3]
        rising = (frequencies - left) / (center - left)
        falling = (right - frequencies) / (right - center)
        filters[index] = np.maximum(0.0, np.minimum(rising, falling)).astype(np.float32)
    return filters


def log_mel(
    stereo_samples: np.ndarray,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = FFT_SIZE,
    hop_size: int = HOP_SIZE,
) -> np.ndarray:
    if sample_rate != SAMPLE_RATE:
        raise ValueError("logmel-v1 requires 48 kHz input")
    if stereo_samples.ndim != 2 or stereo_samples.shape[1] != 2:
        raise ValueError("logmel-v1 requires [frames, 2] stereo input")
    mono = stereo_samples.astype(np.float32).mean(axis=1)
    if len(mono) < fft_size:
        return np.empty((0, MEL_BINS), dtype=np.float32)
    frame_count = 1 + (len(mono) - fft_size) // hop_size
    shape = (frame_count, fft_size)
    strides = (mono.strides[0] * hop_size, mono.strides[0])
    frames = np.lib.stride_tricks.as_strided(mono, shape=shape, strides=strides)
    # Must match C++ MakeHannWindow: symmetric N-1 denominator.
    window = np.hanning(fft_size).astype(np.float32)
    spectrum = np.fft.rfft(frames * window, axis=1)
    power = (spectrum.real**2 + spectrum.imag**2).astype(np.float32) / float(fft_size * fft_size)
    mel = power @ mel_filterbank(sample_rate, fft_size).T
    return np.log1p(10_000.0 * np.maximum(mel, 0.0)).astype(np.float32)


def stereo_mel_energy(
    stereo_samples: np.ndarray,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = FFT_SIZE,
    hop_size: int = HOP_SIZE,
) -> np.ndarray:
    if sample_rate != SAMPLE_RATE:
        raise ValueError("stereo-pcen-v2 requires 48 kHz input")
    if stereo_samples.ndim != 2 or stereo_samples.shape[1] != 2:
        raise ValueError("stereo-pcen-v2 requires [frames, 2] stereo input")
    if len(stereo_samples) < fft_size:
        return np.empty((0, MEL_BINS), dtype=np.float32)

    samples = stereo_samples.astype(np.float32, copy=False)
    frame_count = 1 + (len(samples) - fft_size) // hop_size
    shape = (frame_count, fft_size, 2)
    strides = (samples.strides[0] * hop_size, samples.strides[0], samples.strides[1])
    frames = np.lib.stride_tricks.as_strided(samples, shape=shape, strides=strides)
    window = np.hanning(fft_size).astype(np.float32)
    spectrum = np.fft.rfft(frames * window[None, :, None], axis=1)
    power = np.mean(spectrum.real**2 + spectrum.imag**2, axis=2).astype(np.float32)
    power /= float(fft_size * fft_size)
    return np.maximum(power @ mel_filterbank(sample_rate, fft_size).T, 0.0).astype(np.float32)


def stereo_pcen_from_energy(
    energy: np.ndarray,
    gain_db: float = 0.0,
    smoothing: float = PCEN_SMOOTHING,
    alpha: float = PCEN_ALPHA,
    delta: float = PCEN_DELTA,
    root: float = PCEN_ROOT,
    epsilon: float = PCEN_EPSILON,
) -> np.ndarray:
    if energy.ndim != 2 or energy.shape[1] != MEL_BINS:
        raise ValueError("mel energy must have shape [frames, 64]")
    if not (0.0 < smoothing <= 1.0 and 0.0 <= alpha <= 1.0 and delta > 0.0
            and 0.0 < root <= 1.0 and epsilon > 0.0):
        raise ValueError("invalid PCEN parameters")
    scaled = energy.astype(np.float32, copy=False) * np.float32(10.0 ** (gain_db / 10.0))

    smooth = np.empty_like(scaled)
    if len(scaled):
        smooth[0] = scaled[0]
        for index in range(1, len(scaled)):
            smooth[index] = (1.0 - smoothing) * smooth[index - 1] + smoothing * scaled[index]
    pcen = np.power(scaled / np.power(epsilon + smooth, alpha) + delta, root) - delta**root
    energy_db = 10.0 * np.log10(np.maximum(scaled, 1e-10))
    absolute = np.clip(energy_db, -100.0, 0.0)
    return np.stack((pcen, absolute), axis=1).astype(np.float32)


def stereo_pcen(
    stereo_samples: np.ndarray,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = FFT_SIZE,
    hop_size: int = HOP_SIZE,
    smoothing: float = PCEN_SMOOTHING,
    alpha: float = PCEN_ALPHA,
    delta: float = PCEN_DELTA,
    root: float = PCEN_ROOT,
    epsilon: float = PCEN_EPSILON,
) -> np.ndarray:
    """Return causal stereo-energy PCEN and absolute-energy feature planes."""
    energy = stereo_mel_energy(stereo_samples, sample_rate, fft_size, hop_size)
    return stereo_pcen_from_energy(energy, 0.0, smoothing, alpha, delta, root, epsilon)


def recognition_features(stereo_samples: np.ndarray, preprocessing_version: str) -> np.ndarray:
    if preprocessing_version == "logmel-v1":
        return log_mel(stereo_samples)[:, None, :]
    if preprocessing_version == "stereo-pcen-v2":
        return stereo_pcen(stereo_samples)
    raise ValueError(f"unsupported preprocessing version: {preprocessing_version}")
