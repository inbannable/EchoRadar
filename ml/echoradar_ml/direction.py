from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import shutil
from typing import Iterable, Protocol

import numpy as np

from . import SAMPLE_RATE
from .audio import Audio, load_pcm_wav, to_stereo_48k, write_pcm16_wav
from .spatial import SpatialParameters


FEATURE_SCHEMA_VERSION = 2
BAND_COUNT = 24
BAND_EDGES_HZ = np.geomspace(120.0, 20000.0, BAND_COUNT + 1, dtype=np.float64)


@dataclass(frozen=True)
class DirectionFeatures:
    schema_version: int
    broadband_ild_db: float
    gcc_delay_samples: float
    gcc_peak: float
    gcc_sharpness: float
    gcc_peak_to_sidelobe: float
    peak_to_noise_db: float
    active_frame_fraction: float
    stereo_quality: float
    rms: float
    band_ild_db: tuple[float, ...]
    band_coherence: tuple[float, ...]
    left_spectral_shape: tuple[float, ...]
    right_spectral_shape: tuple[float, ...]

    def vector(self) -> np.ndarray:
        return np.asarray(
            (
                self.schema_version,
                self.broadband_ild_db,
                self.gcc_delay_samples,
                self.gcc_peak,
                self.gcc_sharpness,
                self.gcc_peak_to_sidelobe,
                self.peak_to_noise_db,
                self.active_frame_fraction,
                self.stereo_quality,
                self.rms,
                *self.band_ild_db,
                *self.band_coherence,
                *self.left_spectral_shape,
                *self.right_spectral_shape,
            ),
            dtype=np.float32,
        )


@dataclass(frozen=True)
class DirectionCorpusRow:
    relative_path: str
    source_path: str
    source_sha256: str
    source_split: str
    sound_class: str
    azimuth_degrees: float
    elevation_degrees: float
    distance_meters: float
    renderer: str
    condition: str
    ground_truth_peak_sample: int
    selected_clip_start_sample: int
    selected_clip_end_sample: int
    peak_accepted: bool
    features: DirectionFeatures


@dataclass(frozen=True)
class DirectionEstimate:
    angle_degrees: float
    confidence: float
    probabilities: tuple[float, ...]


@dataclass(frozen=True)
class DirectionEvaluationReport:
    count: int
    median_error_degrees: float
    p90_error_degrees: float
    maximum_error_degrees: float
    left_right_accuracy: float
    front_rear_accuracy: float
    high_confidence_catastrophic_rate: float
    mean_confidence: float
    confidence_ece: float
    peak_selection_success_rate: float
    peak_rejection_rate: float
    success_gate_passed: bool
    per_class: dict[str, dict[str, float]]
    per_angle: dict[str, dict[str, float]]
    per_condition: dict[str, dict[str, float]]


class DirectionRenderer(Protocol):
    def render(self, samples: np.ndarray, parameters: SpatialParameters) -> np.ndarray: ...


def _safe_db_ratio(numerator: float, denominator: float) -> float:
    return float(10.0 * np.log10((numerator + 1.0e-12) / (denominator + 1.0e-12)))


def _envelope(samples: np.ndarray, smoothing_frames: int) -> tuple[np.ndarray, np.ndarray, int, float, float]:
    power = 0.5 * np.sum(np.square(samples.astype(np.float64)), axis=1)
    smoothing_frames = int(np.clip(smoothing_frames, 1, len(samples)))
    before = smoothing_frames // 2
    after = smoothing_frames - before
    prefix = np.concatenate((np.zeros(1, dtype=np.float64), np.cumsum(power)))
    rms = np.empty(len(samples), dtype=np.float64)
    for frame in range(len(samples)):
        start = max(0, frame - before)
        end = min(len(samples), frame + after)
        rms[frame] = np.sqrt(max(0.0, (prefix[end] - prefix[start]) / (end - start)))
    peak_index = int(np.argmax(rms))
    peak = float(rms[peak_index])
    # C++ nth_element at n/5 selects this zero-based order statistic.
    noise = float(np.partition(rms, len(rms) // 5)[len(rms) // 5])
    normalized = np.clip((rms - noise) / max(1.0e-9, peak - noise), 0.0, 1.0)
    weights = np.square(normalized)
    peak_to_noise_db = float(20.0 * np.log10((peak + 1.0e-9) / (noise + 1.0e-9)))
    active_fraction = float(np.mean(normalized >= 0.2))
    return rms, weights, peak_index, peak_to_noise_db, active_fraction


@dataclass(frozen=True)
class PeakWindow:
    peak_frame: int
    start_frame: int
    end_frame: int
    peak_to_noise_db: float
    active_frame_fraction: float


def select_peak_window(
    samples: np.ndarray,
    *,
    before_peak_ms: int,
    after_peak_ms: int,
    smoothing_ms: int = 4,
    minimum_peak_to_noise_db: float = 6.0,
    minimum_active_frame_fraction: float = 0.02,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = 1024,
) -> tuple[np.ndarray, PeakWindow]:
    stereo = np.asarray(samples, dtype=np.float32)
    if stereo.ndim != 2 or stereo.shape[1] != 2 or not len(stereo):
        raise ValueError("peak search input must be stereo")
    _, _, peak, peak_to_noise_db, active_fraction = _envelope(
        stereo, max(1, smoothing_ms * sample_rate // 1000)
    )
    start = max(0, peak - before_peak_ms * sample_rate // 1000)
    end = min(len(stereo), peak + after_peak_ms * sample_rate // 1000)
    selection = PeakWindow(peak, start, end, peak_to_noise_db, active_fraction)
    if peak_to_noise_db < minimum_peak_to_noise_db:
        raise ValueError("direction peak is not sufficiently above the local noise floor")
    if active_fraction < minimum_active_frame_fraction or end - start < fft_size:
        raise ValueError("direction peak has insufficient active-frame coverage")
    return stereo[start:end], selection


def extract_direction_features(
    samples: np.ndarray,
    sample_rate: int = SAMPLE_RATE,
    fft_size: int = 1024,
    hop_size: int = 240,
    maximum_lag_samples: int = 32,
) -> DirectionFeatures:
    """Extract the v2 peak-masked stereo cues used by the native estimator.

    This is deliberately a feature/corpus reference, not a promise that HRTF
    rendering has an analytic inverse. Real-device samples still belong in the
    guided calibration profile.
    """
    stereo = np.asarray(samples, dtype=np.float32)
    if stereo.ndim != 2 or stereo.shape[1] != 2 or len(stereo) < fft_size:
        raise ValueError("direction input must be stereo and contain at least one FFT window")
    left = stereo[:, 0].astype(np.float64)
    right = stereo[:, 1].astype(np.float64)
    _, activity_weights, _, peak_to_noise_db, active_fraction = _envelope(
        stereo, max(1, sample_rate * 4 // 1000)
    )
    left_energy = float(np.dot(activity_weights, np.square(left)))
    right_energy = float(np.dot(activity_weights, np.square(right)))
    signal_rms = float(np.sqrt(
        (left_energy + right_energy) / max(1.0, 2.0 * float(np.sum(activity_weights)))
    ))
    if signal_rms < 1.0e-6:
        raise ValueError("direction input is silent")

    starts = np.arange(0, len(stereo) - fft_size + 1, hop_size)
    window = np.hanning(fft_size).astype(np.float64)
    frequencies = np.fft.rfftfreq(fft_size, 1.0 / sample_rate)
    left_bands = np.zeros(BAND_COUNT, dtype=np.float64)
    right_bands = np.zeros(BAND_COUNT, dtype=np.float64)
    cross_bands = np.zeros(BAND_COUNT, dtype=np.complex128)
    cross_spectrum = np.zeros(fft_size // 2 + 1, dtype=np.complex128)
    for start in starts:
        frame_weight = float(activity_weights[min(len(stereo) - 1, start + fft_size // 2)])
        if frame_weight <= 1.0e-6:
            continue
        left_spectrum = np.fft.rfft(left[start:start + fft_size] * window)
        right_spectrum = np.fft.rfft(right[start:start + fft_size] * window)
        frame_cross = left_spectrum * np.conj(right_spectrum)
        cross_spectrum += frame_weight * frame_cross
        for band in range(BAND_COUNT):
            mask = (frequencies >= BAND_EDGES_HZ[band]) & (frequencies < BAND_EDGES_HZ[band + 1])
            left_bands[band] += frame_weight * float(np.sum(np.square(np.abs(left_spectrum[mask]))))
            right_bands[band] += frame_weight * float(np.sum(np.square(np.abs(right_spectrum[mask]))))
            cross_bands[band] += frame_weight * np.sum(frame_cross[mask])

    maximum_lag = min(maximum_lag_samples, len(stereo) // 4)
    valid_bins = np.abs(cross_spectrum[1:-1]) > 1.0e-12
    bins = np.arange(1, len(cross_spectrum) - 1, dtype=np.float64)[valid_bins]
    phat = cross_spectrum[1:-1][valid_bins]
    phat /= np.abs(phat)
    gcc = np.asarray([
        float(np.real(np.sum(phat * np.exp(2j * np.pi * bins * lag / fft_size))) /
              max(1, len(phat)))
        for lag in range(-maximum_lag, maximum_lag + 1)
    ])
    best_index = int(np.argmax(gcc))
    best_lag = best_index - maximum_lag
    refined_lag = float(best_lag)
    neighbor_mean = float(gcc[best_index])
    if 0 < best_index < len(gcc) - 1:
        previous, center, following = gcc[best_index - 1:best_index + 2]
        neighbor_mean = float(0.5 * (previous + following))
        curvature = previous - 2.0 * center + following
        if abs(curvature) > 1.0e-7:
            refined_lag += float(np.clip(
                0.5 * (previous - following) / curvature, -0.5, 0.5
            ))
    sidelobes = [value for index, value in enumerate(gcc) if abs(index - best_index) > 2]
    sidelobe = float(max(sidelobes, default=-1.0))
    gcc_peak = float(np.clip(gcc[best_index], -1.0, 1.0))
    gcc_sharpness = float(np.clip(
        (gcc[best_index] - neighbor_mean) / (abs(gcc[best_index]) + 1.0e-6), 0.0, 1.0
    ))
    gcc_peak_to_sidelobe = float(np.clip(
        (gcc[best_index] - sidelobe) / (abs(gcc[best_index]) + 1.0e-6), 0.0, 1.0
    ))

    band_ild = np.asarray([
        _safe_db_ratio(left_bands[index], right_bands[index]) for index in range(BAND_COUNT)
    ])
    band_coherence = np.abs(cross_bands) / np.sqrt(np.maximum(1.0e-18, left_bands * right_bands))
    band_coherence = np.clip(band_coherence, 0.0, 1.0)
    left_shape = left_bands / max(1.0e-18, float(np.sum(left_bands)))
    right_shape = right_bands / max(1.0e-18, float(np.sum(right_bands)))
    energy_weights = (left_bands + right_bands) / max(
        1.0e-18, float(np.sum(left_bands + right_bands))
    )
    coherence_mean = float(np.sum(energy_weights * band_coherence))
    peak_clarity = float(np.clip((peak_to_noise_db - 3.0) / 18.0, 0.0, 1.0))
    coverage = float(np.clip(active_fraction / 0.18, 0.0, 1.0))
    gcc_quality = float(np.sqrt(gcc_sharpness * gcc_peak_to_sidelobe))
    stereo_quality = float(np.clip(
        0.27 * peak_clarity + 0.23 * coverage + 0.25 * coherence_mean + 0.25 * gcc_quality,
        0.0, 1.0,
    ))
    return DirectionFeatures(
        schema_version=FEATURE_SCHEMA_VERSION,
        broadband_ild_db=_safe_db_ratio(left_energy, right_energy),
        gcc_delay_samples=refined_lag,
        gcc_peak=gcc_peak,
        gcc_sharpness=gcc_sharpness,
        gcc_peak_to_sidelobe=gcc_peak_to_sidelobe,
        peak_to_noise_db=peak_to_noise_db,
        active_frame_fraction=active_fraction,
        stereo_quality=stereo_quality,
        rms=signal_rms,
        band_ild_db=tuple(float(value) for value in band_ild),
        band_coherence=tuple(float(value) for value in band_coherence),
        left_spectral_shape=tuple(float(value) for value in left_shape),
        right_spectral_shape=tuple(float(value) for value in right_shape),
    )


def circular_error_degrees(estimate: float, target: float) -> float:
    return abs((estimate - target + 180.0) % 360.0 - 180.0)


def _add_circular_kernel(probabilities: np.ndarray, angle: float, sigma: float, weight: float) -> None:
    centers = np.arange(24, dtype=np.float64) * 15.0
    distances = np.abs((centers - angle + 180.0) % 360.0 - 180.0)
    probabilities += weight * np.exp(-0.5 * np.square(distances / max(5.0, sigma)))


def map_direction_features(features: DirectionFeatures, isolation_percent: float = 0.0) -> DirectionEstimate:
    """Python reference for deterministic DirectionMapper v2."""
    ild_values = np.asarray(features.band_ild_db, dtype=np.float64)
    coherence = np.asarray(features.band_coherence, dtype=np.float64)
    weights = 0.05 + np.square(coherence[3:-1])
    band_ild = float(np.sum(weights * np.clip(ild_values[3:-1], -30.0, 30.0)) /
                     max(0.001, float(np.sum(weights))))
    ild = 0.35 * features.broadband_ild_db + 0.65 * band_ild
    isolation = float(np.clip(isolation_percent / 100.0, 0.0, 1.0))
    level_evidence = float(np.tanh(-ild / (9.0 * (1.0 + 0.65 * isolation))))
    time_evidence = float(np.tanh(features.gcc_delay_samples / 11.0))
    level_reliability = float(np.clip(abs(ild) / 8.0, 0.0, 1.0))
    time_reliability = float(np.clip(
        0.55 * features.gcc_sharpness + 0.45 * features.gcc_peak_to_sidelobe, 0.0, 1.0
    ))
    conflicting = level_evidence * time_evidence < -0.04
    if conflicting:
        if level_reliability >= time_reliability:
            time_reliability *= 0.2
        else:
            level_reliability *= 0.2
    side = float(np.clip(
        (level_reliability * level_evidence + time_reliability * time_evidence) /
        max(0.001, level_reliability + time_reliability), -1.0, 1.0
    ))
    left_shape = np.asarray(features.left_spectral_shape)
    right_shape = np.asarray(features.right_spectral_shape)
    average_shape = 0.5 * (left_shape + right_shape)
    front_rear_evidence = float(np.tanh(2.5 * (
        float(np.sum(average_shape[6:17])) - float(np.sum(average_shape[17:]))
    )))
    ear_shape_difference = float(np.sum(np.abs(left_shape - right_shape)))
    front_angle = (side * 90.0) % 360.0
    rear_angle = (180.0 - side * 90.0) % 360.0
    probabilities = np.zeros(24, dtype=np.float64)
    front_weight = 0.5 + 0.42 * front_rear_evidence
    sigma = 16.0 + (1.0 - features.stereo_quality) * 42.0
    _add_circular_kernel(probabilities, front_angle, sigma, front_weight)
    _add_circular_kernel(probabilities, rear_angle, sigma, 1.0 - front_weight)
    probabilities /= np.sum(probabilities)
    primary = int(np.argmax(probabilities))
    interaural = float(np.clip(
        0.55 * abs(level_evidence) + 0.45 * abs(time_evidence), 0.0, 1.0
    ))
    spectral_reliability = float(np.clip(ear_shape_difference * 1.5, 0.0, 1.0))
    front_rear_reliability = abs(front_rear_evidence) * max(interaural, spectral_reliability)
    directional = float(np.clip(
        (0.55 if conflicting else 1.0) * max(interaural, 0.7 * front_rear_reliability),
        0.0, 1.0,
    ))
    entropy = float(-np.sum(probabilities * np.log(np.maximum(probabilities, 1.0e-12))) /
                    np.log(24.0))
    confidence = float(np.clip(
        features.stereo_quality * directional * (0.35 + 0.65 * (1.0 - entropy)), 0.0, 1.0
    ))
    return DirectionEstimate(primary * 15.0, confidence,
                             tuple(float(value) for value in probabilities))


def evaluate_direction_rows(
    rows: Iterable[DirectionCorpusRow],
    *,
    audio_root: Path | None = None,
    error_clip_directory: Path | None = None,
) -> DirectionEvaluationReport:
    evaluated = []
    for row in rows:
        estimate = map_direction_features(row.features)
        error = circular_error_degrees(estimate.angle_degrees, row.azimuth_degrees)
        evaluated.append((row, estimate, error))
    if not evaluated:
        raise ValueError("direction evaluation requires at least one row")
    errors = np.asarray([item[2] for item in evaluated], dtype=np.float64)

    def side(angle: float) -> int:
        sine = np.sin(np.deg2rad(angle))
        return 0 if abs(sine) < 1.0e-6 else (1 if sine > 0 else -1)

    def front(angle: float) -> int:
        cosine = np.cos(np.deg2rad(angle))
        return 0 if abs(cosine) < 1.0e-6 else (1 if cosine > 0 else -1)

    left_right = np.mean([
        side(estimate.angle_degrees) == side(row.azimuth_degrees)
        for row, estimate, _ in evaluated
    ])
    front_rear = np.mean([
        front(estimate.angle_degrees) == front(row.azimuth_degrees)
        for row, estimate, _ in evaluated
    ])
    high_confidence = [item for item in evaluated if item[1].confidence >= 0.6]
    catastrophic = [error > 90.0 for _, _, error in high_confidence]

    calibration_error = 0.0
    for lower in np.arange(0.0, 1.0, 0.1):
        bucket = [item for item in evaluated
                  if lower <= item[1].confidence < lower + 0.1]
        if not bucket:
            continue
        accuracy = float(np.mean([item[2] <= 30.0 for item in bucket]))
        confidence = float(np.mean([item[1].confidence for item in bucket]))
        calibration_error += len(bucket) / len(evaluated) * abs(accuracy - confidence)

    if error_clip_directory is not None:
        if audio_root is None:
            raise ValueError("audio_root is required when saving error clips")
        error_clip_directory.mkdir(parents=True, exist_ok=True)
        for row, estimate, error in evaluated:
            if error <= 30.0:
                continue
            source = audio_root / row.relative_path
            if source.is_file():
                destination = error_clip_directory / (
                    f"err-{int(round(error)):03d}_conf-{estimate.confidence:.2f}_"
                    f"{Path(row.relative_path).name}"
                )
                shutil.copy2(source, destination)

    def breakdown(key) -> dict[str, dict[str, float]]:
        groups: dict[str, list[float]] = {}
        for row, _, error in evaluated:
            groups.setdefault(str(key(row)), []).append(error)
        return {
            name: {"count": float(len(values)), "median_error_degrees": float(np.median(values)),
                   "p90_error_degrees": float(np.percentile(values, 90))}
            for name, values in sorted(groups.items())
        }

    median = float(np.median(errors))
    p90 = float(np.percentile(errors, 90))
    catastrophic_rate = float(np.mean(catastrophic)) if catastrophic else 0.0
    peak_success = float(np.mean([row.peak_accepted for row, _, _ in evaluated]))
    return DirectionEvaluationReport(
        count=len(evaluated), median_error_degrees=median,
        p90_error_degrees=p90, maximum_error_degrees=float(np.max(errors)),
        left_right_accuracy=float(left_right), front_rear_accuracy=float(front_rear),
        high_confidence_catastrophic_rate=catastrophic_rate,
        mean_confidence=float(np.mean([item[1].confidence for item in evaluated])),
        confidence_ece=float(calibration_error),
        peak_selection_success_rate=peak_success,
        peak_rejection_rate=1.0 - peak_success,
        success_gate_passed=median <= 15.0 and p90 <= 30.0 and not any(catastrophic),
        per_class=breakdown(lambda row: row.sound_class),
        per_angle=breakdown(lambda row: int(round(row.azimuth_degrees)) % 360),
        per_condition=breakdown(lambda row: row.condition),
    )


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
    conditions: Iterable[str] = ("clean",),
) -> list[Path]:
    """Render deterministic known-bearing clips and an auditable JSONL manifest."""
    output.mkdir(parents=True, exist_ok=True)
    rendered_paths: list[Path] = []
    rows: list[DirectionCorpusRow] = []
    for source_index, source_path in enumerate(sorted(Path(path) for path in source_paths)):
        source_bytes = source_path.read_bytes()
        source_hash = hashlib.sha256(source_bytes).hexdigest()
        source_split = "validation" if int(source_hash[:8], 16) % 5 == 0 else "train"
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
            clean = np.asarray(renderer.render(source, parameters), dtype=np.float32)
            for condition in conditions:
                rendered = _apply_corpus_condition(clean, condition, source_index, wrapped)
                name = (f"source-{source_index:05d}_az-{int(round(wrapped)):03d}_"
                        f"{condition}.wav")
                destination = output / name
                write_pcm16_wav(destination, Audio(rendered, SAMPLE_RATE))
                _, _, ground_truth_peak, _, _ = _envelope(rendered, SAMPLE_RATE * 4 // 1000)
                sound_class = _source_class(source_path)
                before, after = (8, 75) if sound_class == "gunshot" else (18, 150)
                try:
                    selected, peak = select_peak_window(
                        rendered, before_peak_ms=before, after_peak_ms=after,
                        minimum_peak_to_noise_db=0.0,
                        minimum_active_frame_fraction=0.001,
                    )
                    accepted = True
                except ValueError:
                    selected = rendered
                    peak = PeakWindow(ground_truth_peak, 0, len(rendered), 0.0, 0.0)
                    accepted = False
                rows.append(DirectionCorpusRow(
                    relative_path=name, source_path=str(source_path), source_sha256=source_hash,
                    source_split=source_split,
                    sound_class=sound_class, azimuth_degrees=wrapped,
                    elevation_degrees=elevation_degrees, distance_meters=distance_meters,
                    renderer="steam-audio-v4.8.1", condition=condition,
                    ground_truth_peak_sample=ground_truth_peak,
                    selected_clip_start_sample=peak.start_frame,
                    selected_clip_end_sample=peak.end_frame,
                    peak_accepted=accepted, features=extract_direction_features(selected),
                ))
                rendered_paths.append(destination)

    manifest = output / "direction-manifest.jsonl"
    with manifest.open("w", encoding="utf-8") as handle:
        for row in rows:
            payload = asdict(row)
            handle.write(json.dumps(payload, sort_keys=True) + "\n")
    return rendered_paths


def _apply_corpus_condition(clean: np.ndarray, condition: str,
                            source_index: int, angle: float) -> np.ndarray:
    result = np.array(clean, dtype=np.float32, copy=True)
    if condition == "clean":
        return result
    if condition == "gain":
        return result * np.float32(0.55)
    if condition == "noise":
        seed = source_index * 1009 + int(round(angle)) * 917 + 17
        rng = np.random.default_rng(seed)
        return result + rng.normal(0.0, 0.003, result.shape).astype(np.float32)
    if condition == "mild-reverb":
        dry = np.array(result, copy=True)
        for delay, gain in ((719, 0.18), (1061, 0.11)):
            result[delay:] += dry[:-delay] * np.float32(gain)
        return result
    if condition == "occlusion":
        # Deterministic one-pole low-pass approximation for controlled evaluation.
        for channel in range(2):
            for frame in range(1, len(result)):
                result[frame, channel] = 0.2 * result[frame, channel] + 0.8 * result[frame - 1, channel]
        return result * np.float32(0.7)
    if condition == "channel-isolation":
        mixed = np.array(result, copy=True)
        result[:, 0] = 0.9 * mixed[:, 0] + 0.1 * mixed[:, 1]
        result[:, 1] = 0.9 * mixed[:, 1] + 0.1 * mixed[:, 0]
        return result
    raise ValueError(f"unsupported direction corpus condition: {condition}")
