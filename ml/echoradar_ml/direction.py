from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
from typing import Iterable, Protocol

import numpy as np

from . import SAMPLE_RATE
from .audio import Audio, load_pcm_wav, to_stereo_48k, write_pcm16_wav
from .spatial import SpatialParameters


BAND_EDGES_HZ = np.asarray(
    (120.0, 300.0, 600.0, 1200.0, 2400.0, 4800.0, 8000.0, 12000.0, 20000.0),
    dtype=np.float64,
)


@dataclass(frozen=True)
class DirectionFeatures:
    broadband_ild_db: float
    itd_samples: float
    correlation_peak: float
    stereo_quality: float
    rms: float
    band_ild_db: tuple[float, ...]
    band_coherence: tuple[float, ...]

    def vector(self) -> np.ndarray:
        return np.asarray(
            (
                self.broadband_ild_db,
                self.itd_samples,
                self.correlation_peak,
                self.stereo_quality,
                self.rms,
                *self.band_ild_db,
                *self.band_coherence,
            ),
            dtype=np.float32,
        )


@dataclass(frozen=True)
class DirectionCorpusRow:
    relative_path: str
    source_path: str
    source_sha256: str
    sound_class: str
    azimuth_degrees: float
    elevation_degrees: float
    distance_meters: float
    renderer: str
    features: DirectionFeatures


class DirectionRenderer(Protocol):
    def render(self, samples: np.ndarray, parameters: SpatialParameters) -> np.ndarray: ...


def _safe_db_ratio(numerator: float, denominator: float) -> float:
    return float(10.0 * np.log10((numerator + 1.0e-12) / (denominator + 1.0e-12)))


def extract_direction_features(
    samples: np.ndarray,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = 1024,
    hop_size: int = 240,
    maximum_lag_samples: int = 32,
) -> DirectionFeatures:
    """Extract the pooled stereo cues used by the native baseline.

    This is deliberately a feature/corpus reference, not a promise that HRTF
    rendering has an analytic inverse. Real-device samples still belong in the
    guided calibration profile.
    """
    stereo = np.asarray(samples, dtype=np.float32)
    if stereo.ndim != 2 or stereo.shape[1] != 2 or len(stereo) < fft_size:
        raise ValueError("direction input must be stereo and contain at least one FFT window")
    left = stereo[:, 0].astype(np.float64)
    right = stereo[:, 1].astype(np.float64)
    left_energy = float(np.dot(left, left))
    right_energy = float(np.dot(right, right))
    signal_rms = float(np.sqrt((left_energy + right_energy) / (2.0 * len(stereo))))
    if signal_rms < 1.0e-6:
        raise ValueError("direction input is silent")

    best_correlation = -np.inf
    best_lag = 0
    maximum_lag = min(maximum_lag_samples, len(stereo) // 4)
    for lag in range(-maximum_lag, maximum_lag + 1):
        left_start = -lag if lag < 0 else 0
        right_start = lag if lag > 0 else 0
        count = len(stereo) - abs(lag)
        lag_left = left[left_start:left_start + count]
        lag_right = right[right_start:right_start + count]
        denominator = np.sqrt(max(1.0e-18, float(np.dot(lag_left, lag_left) *
                                                 np.dot(lag_right, lag_right))))
        correlation = float(np.dot(lag_left, lag_right) / denominator)
        if correlation > best_correlation:
            best_correlation = correlation
            best_lag = lag

    starts = np.arange(0, len(stereo) - fft_size + 1, hop_size)
    window = np.hanning(fft_size).astype(np.float64)
    frequencies = np.fft.rfftfreq(fft_size, 1.0 / sample_rate)
    left_bands = np.zeros(8, dtype=np.float64)
    right_bands = np.zeros(8, dtype=np.float64)
    cross_bands = np.zeros(8, dtype=np.complex128)
    for start in starts:
        left_spectrum = np.fft.rfft(left[start:start + fft_size] * window)
        right_spectrum = np.fft.rfft(right[start:start + fft_size] * window)
        for band in range(8):
            mask = (frequencies >= BAND_EDGES_HZ[band]) & (frequencies < BAND_EDGES_HZ[band + 1])
            left_bands[band] += float(np.sum(np.square(np.abs(left_spectrum[mask]))))
            right_bands[band] += float(np.sum(np.square(np.abs(right_spectrum[mask]))))
            cross_bands[band] += np.sum(left_spectrum[mask] * np.conj(right_spectrum[mask]))

    band_ild = np.asarray([
        _safe_db_ratio(left_bands[index], right_bands[index]) for index in range(8)
    ])
    band_coherence = np.abs(cross_bands) / np.sqrt(np.maximum(1.0e-18, left_bands * right_bands))
    band_coherence = np.clip(band_coherence, 0.0, 1.0)
    ild_spread = float(np.std(band_ild))
    asymmetry = max(
        abs(_safe_db_ratio(left_energy, right_energy)) / 9.0,
        abs(float(-best_lag)) / 12.0,
        ild_spread / 8.0,
    )
    stereo_quality = float(np.clip(
        (0.25 + 0.75 * asymmetry) * (0.35 + 0.65 * float(np.mean(band_coherence))),
        0.0,
        1.0,
    ))
    return DirectionFeatures(
        broadband_ild_db=_safe_db_ratio(left_energy, right_energy),
        itd_samples=float(-best_lag),
        correlation_peak=float(np.clip(best_correlation, -1.0, 1.0)),
        stereo_quality=stereo_quality,
        rms=signal_rms,
        band_ild_db=tuple(float(value) for value in band_ild),
        band_coherence=tuple(float(value) for value in band_coherence),
    )


def circular_error_degrees(estimate: float, target: float) -> float:
    return abs((estimate - target + 180.0) % 360.0 - 180.0)


def _source_class(path: Path) -> str:
    text = " ".join(part.lower() for part in path.parts)
    if "footstep" in text or "step" in text:
        return "footstep"
    if "gunshot" in text or "weapon" in text or "fire" in text:
        return "gunshot"
    return "other"


def generate_direction_corpus(
    source_paths: Iterable[Path],
    output: Path,
    renderer: DirectionRenderer,
    azimuths: Iterable[float] = tuple(float(angle) for angle in range(0, 360, 15)),
    elevation_degrees: float = 0.0,
    distance_meters: float = 4.0,
) -> list[Path]:
    """Render deterministic known-bearing clips and an auditable JSONL manifest."""
    output.mkdir(parents=True, exist_ok=True)
    rendered_paths: list[Path] = []
    rows: list[DirectionCorpusRow] = []
    for source_index, source_path in enumerate(sorted(Path(path) for path in source_paths)):
        source_bytes = source_path.read_bytes()
        source_hash = hashlib.sha256(source_bytes).hexdigest()
        source = to_stereo_48k(load_pcm_wav(source_path)).samples
        for angle in azimuths:
            wrapped = float(angle % 360.0)
            parameters = SpatialParameters(
                azimuth_degrees=wrapped,
                elevation_degrees=elevation_degrees,
                distance_meters=distance_meters,
                occlusion=0.0,
                transmission_low=1.0,
                transmission_mid=1.0,
                transmission_high=1.0,
                directivity=0.0,
                reverb_mix=0.0,
            )
            rendered = np.asarray(renderer.render(source, parameters), dtype=np.float32)
            name = f"source-{source_index:05d}_az-{int(round(wrapped)):03d}.wav"
            destination = output / name
            write_pcm16_wav(destination, Audio(rendered, SAMPLE_RATE))
            rows.append(DirectionCorpusRow(
                relative_path=name,
                source_path=str(source_path),
                source_sha256=source_hash,
                sound_class=_source_class(source_path),
                azimuth_degrees=wrapped,
                elevation_degrees=elevation_degrees,
                distance_meters=distance_meters,
                renderer="steam-audio-v4.8.1",
                features=extract_direction_features(rendered),
            ))
            rendered_paths.append(destination)

    manifest = output / "direction-manifest.jsonl"
    with manifest.open("w", encoding="utf-8") as handle:
        for row in rows:
            payload = asdict(row)
            handle.write(json.dumps(payload, sort_keys=True) + "\n")
    return rendered_paths
