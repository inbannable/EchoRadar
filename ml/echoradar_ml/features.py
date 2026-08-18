from __future__ import annotations

import numpy as np

from . import (
    FFT_SIZE, HOP_SIZE, MEL_BINS,
    PCEN_ALPHA, PCEN_DELTA, PCEN_EPSILON, PCEN_ROOT, PCEN_SMOOTHING,
    PREPROCESSING_VERSION, SAMPLE_RATE,
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
        left, center, right = points[index:index + 3]
        rising = (frequencies - left) / (center - left)
        falling = (right - frequencies) / (right - center)
        filters[index] = np.maximum(0.0, np.minimum(rising, falling)).astype(np.float32)
    return filters


def _stereo_spectrum(stereo_samples: np.ndarray, sample_rate: int, fft_size: int,
                     hop_size: int) -> np.ndarray:
    if sample_rate != SAMPLE_RATE:
        raise ValueError("recognition features require 48 kHz input")
    if stereo_samples.ndim != 2 or stereo_samples.shape[1] != 2:
        raise ValueError("recognition features require [frames, 2] stereo input")
    if len(stereo_samples) < fft_size:
        return np.empty((0, fft_size // 2 + 1, 2), dtype=np.complex64)
    samples = stereo_samples.astype(np.float32, copy=False)
    frame_count = 1 + (len(samples) - fft_size) // hop_size
    shape = (frame_count, fft_size, 2)
    strides = (samples.strides[0] * hop_size, samples.strides[0], samples.strides[1])
    frames = np.lib.stride_tricks.as_strided(samples, shape=shape, strides=strides)
    window = np.hanning(fft_size).astype(np.float32)
    return np.fft.rfft(frames * window[None, :, None], axis=1).astype(np.complex64)


def log_mel(
    stereo_samples: np.ndarray,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = FFT_SIZE,
    hop_size: int = HOP_SIZE,
) -> np.ndarray:
    if sample_rate != SAMPLE_RATE:
        raise ValueError("recognition features require 48 kHz input")
    if stereo_samples.ndim != 2 or stereo_samples.shape[1] != 2:
        raise ValueError("recognition features require [frames, 2] stereo input")
    mono = np.ascontiguousarray(stereo_samples.astype(np.float32).mean(axis=1))
    if len(mono) < fft_size:
        return np.empty((0, MEL_BINS), dtype=np.float32)
    frame_count = 1 + (len(mono) - fft_size) // hop_size
    frames = np.lib.stride_tricks.as_strided(
        mono, shape=(frame_count, fft_size),
        strides=(mono.strides[0] * hop_size, mono.strides[0]),
    )
    mono_spectrum = np.fft.rfft(
        frames * np.hanning(fft_size).astype(np.float32)[None, :], axis=1,
    )
    power = (mono_spectrum.real**2 + mono_spectrum.imag**2).astype(np.float32)
    power /= float(fft_size * fft_size)
    mel = power @ mel_filterbank(sample_rate, fft_size).T
    return np.log1p(10_000.0 * np.maximum(mel, 0.0)).astype(np.float32)


def stereo_mel_energy(
    stereo_samples: np.ndarray,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = FFT_SIZE,
    hop_size: int = HOP_SIZE,
) -> np.ndarray:
    spectrum = _stereo_spectrum(stereo_samples, sample_rate, fft_size, hop_size)
    if not len(spectrum):
        return np.empty((0, MEL_BINS), dtype=np.float32)
    power = np.mean(spectrum.real**2 + spectrum.imag**2, axis=2).astype(np.float32)
    power /= float(fft_size * fft_size)
    return np.maximum(power @ mel_filterbank(sample_rate, fft_size).T, 0.0).astype(np.float32)


def _pcen_planes(
    energy: np.ndarray,
    gain_db: float,
    smoothing: float,
    alpha: float,
    delta: float,
    root: float,
    epsilon: float,
) -> tuple[np.ndarray, np.ndarray]:
    scaled = energy.astype(np.float32, copy=False) * np.float32(10.0 ** (gain_db / 10.0))
    smooth = np.empty_like(scaled)
    if len(scaled):
        smooth[0] = scaled[0]
        for index in range(1, len(scaled)):
            smooth[index] = (1.0 - smoothing) * smooth[index - 1] + smoothing * scaled[index]
    pcen = np.power(scaled / np.power(epsilon + smooth, alpha) + delta, root) - delta**root
    absolute = np.clip(10.0 * np.log10(np.maximum(scaled, 1e-10)), -100.0, 0.0)
    return pcen.astype(np.float32), absolute.astype(np.float32)


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
    pcen, absolute = _pcen_planes(energy, gain_db, smoothing, alpha, delta, root, epsilon)
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
    energy = stereo_mel_energy(stereo_samples, sample_rate, fft_size, hop_size)
    return stereo_pcen_from_energy(energy, 0.0, smoothing, alpha, delta, root, epsilon)


def stereo_onset_features(
    stereo_samples: np.ndarray,
    gain_db: float = 0.0,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = FFT_SIZE,
    hop_size: int = HOP_SIZE,
) -> np.ndarray:
    """Return current ``[time, 5, mel]`` recognition and spatial planes.

    Planes 0-1 are channel-order invariant PCEN and absolute energy.  Plane 2
    is clipped/scaled ILD.  Planes 3-4 are real/imaginary mel-band coherence,
    retaining phase symmetry without allowing it to erase acoustic energy.
    """
    spectrum = _stereo_spectrum(stereo_samples, sample_rate, fft_size, hop_size)
    if not len(spectrum):
        return np.empty((0, 5, MEL_BINS), dtype=np.float32)
    filters = mel_filterbank(sample_rate, fft_size)
    normalization = float(fft_size * fft_size)
    left_power = ((spectrum[:, :, 0].real**2 + spectrum[:, :, 0].imag**2) /
                  normalization).astype(np.float32)
    right_power = ((spectrum[:, :, 1].real**2 + spectrum[:, :, 1].imag**2) /
                   normalization).astype(np.float32)
    left_mel = np.maximum(left_power @ filters.T, 0.0)
    right_mel = np.maximum(right_power @ filters.T, 0.0)
    mean_energy = 0.5 * (left_mel + right_mel)
    pcen, absolute = _pcen_planes(
        mean_energy, gain_db, PCEN_SMOOTHING, PCEN_ALPHA, PCEN_DELTA,
        PCEN_ROOT, PCEN_EPSILON,
    )

    ild = 10.0 * np.log10((left_mel + 1e-10) / (right_mel + 1e-10))
    ild = np.clip(ild / 30.0, -1.0, 1.0)
    cross = spectrum[:, :, 0] * np.conj(spectrum[:, :, 1]) / normalization
    cross_mel = cross @ filters.T
    coherence_denominator = np.sqrt(left_mel * right_mel) + 1e-10
    coherence = cross_mel / coherence_denominator
    coherence_real = np.clip(coherence.real, -1.0, 1.0)
    coherence_imag = np.clip(coherence.imag, -1.0, 1.0)
    return np.stack((pcen, absolute, ild, coherence_real, coherence_imag), axis=1).astype(np.float32)


def scene_activity(features: np.ndarray, smoothing_frames: int = 100) -> np.ndarray:
    """Compute a causal [0, 1] activity trace from the absolute-energy plane."""
    if features.ndim != 3 or features.shape[1] < 2:
        raise ValueError("features must have shape [time, channels, mel]")
    instantaneous = np.clip((features[:, 1].mean(axis=1) + 80.0) / 60.0, 0.0, 1.0)
    output = np.empty_like(instantaneous, dtype=np.float32)
    alpha = 2.0 / (max(1, smoothing_frames) + 1.0)
    state = 0.0
    for index, value in enumerate(instantaneous):
        state = float(value) if index == 0 else alpha * float(value) + (1.0 - alpha) * state
        output[index] = state
    return output


def recognition_features(
    stereo_samples: np.ndarray,
    preprocessing_version: str = PREPROCESSING_VERSION,
) -> np.ndarray:
    if preprocessing_version != PREPROCESSING_VERSION:
        raise ValueError(f"unsupported preprocessing version: {preprocessing_version}")
    return stereo_onset_features(stereo_samples)
