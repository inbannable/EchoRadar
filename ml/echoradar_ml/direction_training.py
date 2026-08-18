from __future__ import annotations

from dataclasses import dataclass
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import time
from typing import Sequence

import numpy as np

from . import (
    CLASS_NAMES, FFT_SIZE, HOP_SIZE, INPUT_CHANNELS, MEL_BINS, PCEN_ALPHA,
    PCEN_DELTA, PCEN_EPSILON, PCEN_ROOT, PCEN_SMOOTHING, SAMPLE_RATE,
    PREPROCESSING_VERSION,
)
from .audio import load_pcm_wav, to_stereo_48k
from .direction_scenes import (
    DIRECTION_PRE_ANCHOR_MILLISECONDS, DIRECTION_REAL_SCENE_SCHEMA,
    DIRECTION_SCENE_FRAMES, DIRECTION_SCENE_MILLISECONDS, DIRECTION_SCENE_SAMPLES,
    MINIMUM_SOURCE_SEPARATION_DEGREES, load_direction_scene_rows,
)
from .features import stereo_onset_features
from .model import require_torch


DIRECTION_CACHE_VERSION = "direction-feature-cache-v1"
DIRECTION_PACKAGE_VERSION = 1
DIRECTION_PREPROCESSING_VERSION = f"{PREPROCESSING_VERSION}-scene48"
DIRECTION_TRACK_COUNT = 3
DIRECTION_CLASS_COUNT = len(CLASS_NAMES)
DIRECTION_OUTPUT_SHAPE = (DIRECTION_CLASS_COUNT, DIRECTION_TRACK_COUNT, 3)
_PERMUTATIONS = tuple(itertools.permutations(range(DIRECTION_TRACK_COUNT)))


@dataclass(frozen=True)
class PredictedDirection:
    sound_class: str
    azimuth_degrees: float
    elevation_degrees: float
    confidence: float
    track_index: int


@dataclass(frozen=True)
class DirectionMetrics:
    gate: str
    scene_count: int
    two_source_support: int
    three_source_support: int
    real_room_count: int
    real_capture_session_count: int
    real_elevation_band_count: int
    real_profile_count: int
    exact_count_accuracy: float
    exact_count_accuracy_two_source: float
    exact_count_accuracy_three_source: float
    source_precision_within_20_degrees: float
    source_recall_within_20_degrees: float
    source_f1_within_20_degrees: float
    source_precision_within_30_degrees: float
    source_recall_within_30_degrees: float
    source_f1_within_30_degrees: float
    median_matched_error_degrees: float
    p90_matched_error_degrees: float
    p90_elevation_error_degrees: float
    close_exact_count_accuracy: float
    close_p90_matched_error_degrees: float
    high_confidence_error_over_45_rate: float
    disabled_gunshot_inclusion_rate: float
    p95_inference_milliseconds: float
    scene_window_delivery_milliseconds: float
    acceptance_passed: bool


def direction_vector(azimuth_degrees: float, elevation_degrees: float) -> np.ndarray:
    azimuth = math.radians(azimuth_degrees)
    elevation = math.radians(elevation_degrees)
    horizontal = math.cos(elevation)
    return np.asarray(
        [horizontal * math.sin(azimuth), math.sin(elevation), horizontal * math.cos(azimuth)],
        dtype=np.float32,
    )


def vector_direction(vector: np.ndarray) -> tuple[float, float, float]:
    values = np.asarray(vector, dtype=np.float64)
    confidence = float(np.linalg.norm(values))
    if not np.isfinite(confidence) or confidence <= 1.0e-9:
        return 0.0, 0.0, 0.0
    unit = values / confidence
    azimuth = math.degrees(math.atan2(unit[0], unit[2])) % 360.0
    elevation = math.degrees(math.atan2(unit[1], math.hypot(unit[0], unit[2])))
    return float(azimuth), float(elevation), float(np.clip(confidence, 0.0, 1.0))


def scene_target_tensor(row: dict) -> np.ndarray:
    output = np.zeros(DIRECTION_OUTPUT_SHAPE, dtype=np.float32)
    next_track = {name: 0 for name in CLASS_NAMES}
    for target in row["targets"]:
        name = target["sound_class"]
        if name not in CLASS_NAMES:
            raise ValueError(f"unsupported direction target class: {name}")
        track = next_track[name]
        if track >= DIRECTION_TRACK_COUNT:
            raise ValueError(f"too many same-class direction targets for {name}")
        rendered = target["rendered_direction"]
        output[CLASS_NAMES.index(name), track] = direction_vector(
            float(rendered["azimuth_degrees"]), float(rendered["elevation_degrees"])
        )
        next_track[name] += 1
    return output


def _cache_metadata(cache_dir: Path) -> dict:
    path = cache_dir / "cache.json"
    if not path.is_file():
        raise ValueError(f"missing direction cache metadata: {path}")
    metadata = json.loads(path.read_text(encoding="utf-8"))
    required = {
        "cache_version": DIRECTION_CACHE_VERSION,
        "preprocessing_version": DIRECTION_PREPROCESSING_VERSION,
        "feature_frames": DIRECTION_SCENE_FRAMES,
        "input_channels": INPUT_CHANNELS,
        "mel_bins": MEL_BINS,
        "class_order": ",".join(CLASS_NAMES),
        "track_count": DIRECTION_TRACK_COUNT,
    }
    for key, expected in required.items():
        if metadata.get(key) != expected:
            raise ValueError(f"incompatible direction cache {key}: {metadata.get(key)!r}")
    return metadata


def prepare_direction_cache(
    manifest_path: str | Path,
    audio_root: str | Path,
    output_dir: str | Path,
    splits: Sequence[str] = ("train", "dev", "test"),
    feature_dtype: str = "float16",
) -> Path:
    if feature_dtype not in ("float16", "float32"):
        raise ValueError("direction cache feature dtype must be float16 or float32")
    rows = load_direction_scene_rows(manifest_path)
    if any(split not in ("train", "dev", "test") for split in splits):
        raise ValueError("direction cache split must be train, dev, or test")
    root = Path(audio_root)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    manifest_hash = hashlib.sha256(Path(manifest_path).read_bytes()).hexdigest()

    split_summaries: dict[str, object] = {}
    for split in splits:
        selected = [row for row in rows if row["split"] == split]
        if not selected:
            raise ValueError(f"direction manifest contains no {split} scenes")
        split_dir = output / split
        split_dir.mkdir(parents=True, exist_ok=True)
        feature_partial = split_dir / "features.partial.npy"
        target_partial = split_dir / "targets.partial.npy"
        features = np.lib.format.open_memmap(
            feature_partial, mode="w+", dtype=feature_dtype,
            shape=(len(selected), INPUT_CHANNELS, DIRECTION_SCENE_FRAMES, MEL_BINS),
        )
        targets = np.lib.format.open_memmap(
            target_partial, mode="w+", dtype="float32",
            shape=(len(selected), *DIRECTION_OUTPUT_SHAPE),
        )
        counts = np.empty(len(selected), dtype=np.uint8)
        close = np.empty(len(selected), dtype=np.bool_)
        ids: list[str] = []
        for index, row in enumerate(selected):
            wav_path = root / row["relative_path"]
            if not wav_path.is_file():
                raise ValueError(f"missing direction scene WAV: {wav_path}")
            if hashlib.sha256(wav_path.read_bytes()).hexdigest() != row["wav_sha256"]:
                raise ValueError(f"direction scene WAV hash mismatch: {wav_path}")
            audio = to_stereo_48k(load_pcm_wav(wav_path))
            if len(audio.samples) != DIRECTION_SCENE_SAMPLES:
                raise ValueError(f"direction scene has an incompatible sample count: {wav_path}")
            scene_features = stereo_onset_features(audio.samples)
            if (scene_features.shape != (DIRECTION_SCENE_FRAMES, INPUT_CHANNELS, MEL_BINS)
                    or not np.isfinite(scene_features).all()):
                raise ValueError(f"direction scene produced an incompatible feature shape: {wav_path}")
            features[index] = scene_features.transpose(1, 0, 2)
            targets[index] = scene_target_tensor(row)
            counts[index] = int(row["target_count"])
            close[index] = row["stratum"] == "close-15-30"
            ids.append(str(row["scene_id"]))
        features.flush()
        targets.flush()
        del features, targets
        feature_partial.replace(split_dir / "features.npy")
        target_partial.replace(split_dir / "targets.npy")
        np.save(split_dir / "counts.npy", counts, allow_pickle=False)
        np.save(split_dir / "close.npy", close, allow_pickle=False)
        (split_dir / "scene-ids.json").write_text(
            json.dumps(ids, separators=(",", ":")) + "\n", encoding="utf-8"
        )
        split_summaries[split] = {
            "count": len(selected),
            "source_count_histogram": {
                str(value): int(np.sum(counts == value)) for value in range(4)
            },
            "close_count": int(close.sum()),
        }

    metadata = {
        "cache_version": DIRECTION_CACHE_VERSION,
        "preprocessing_version": DIRECTION_PREPROCESSING_VERSION,
        "manifest_sha256": manifest_hash,
        "feature_dtype": feature_dtype,
        "feature_frames": DIRECTION_SCENE_FRAMES,
        "input_channels": INPUT_CHANNELS,
        "mel_bins": MEL_BINS,
        "class_order": ",".join(CLASS_NAMES),
        "track_count": DIRECTION_TRACK_COUNT,
        "splits": split_summaries,
    }
    (output / "cache.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return output


def build_direction_model():
    torch = require_torch()
    nn = torch.nn

    class ConvBlock(nn.Module):
        def __init__(self, input_channels: int, output_channels: int):
            super().__init__()
            self.depthwise = nn.Conv2d(
                input_channels, input_channels, kernel_size=3, padding=1,
                groups=input_channels, bias=False,
            )
            self.pointwise = nn.Conv2d(input_channels, output_channels, kernel_size=1, bias=False)
            groups = 8 if output_channels % 8 == 0 else 4
            self.norm = nn.GroupNorm(groups, output_channels)

        def forward(self, values):
            return torch.nn.functional.silu(self.norm(self.pointwise(self.depthwise(values))))

    class MultiAccdoaDirectionNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.front = nn.Sequential(
                nn.Conv2d(INPUT_CHANNELS, 32, kernel_size=3, padding=1, bias=False),
                nn.GroupNorm(8, 32),
                nn.SiLU(),
                ConvBlock(32, 64),
                nn.MaxPool2d(kernel_size=(1, 4), stride=(1, 4)),
                ConvBlock(64, 96),
                nn.MaxPool2d(kernel_size=(2, 2), stride=(2, 2)),
                ConvBlock(96, 128),
                nn.MaxPool2d(kernel_size=(2, 2), stride=(2, 2)),
            )
            self.projection = nn.Linear(128 * 4, 160)
            self.recurrent = nn.GRU(
                160, 128, num_layers=2, batch_first=True, bidirectional=True,
                dropout=0.10,
            )
            self.head = nn.Sequential(
                nn.Linear(256, 128), nn.SiLU(),
                nn.Linear(128, DIRECTION_CLASS_COUNT * DIRECTION_TRACK_COUNT * 3),
            )

        def forward(self, values):
            if values.ndim != 4:
                raise ValueError("direction model input must be [batch, 5, 48, 64]")
            normalized = torch.cat((
                values[:, :1], (values[:, 1:2] + 100.0) / 100.0, values[:, 2:]
            ), dim=1)
            encoded = self.front(normalized)
            encoded = encoded.permute(0, 2, 1, 3).flatten(2)
            encoded = torch.nn.functional.silu(self.projection(encoded))
            encoded, _ = self.recurrent(encoded)
            pooled = encoded.mean(dim=1)
            return torch.tanh(self.head(pooled)).reshape(
                values.shape[0], DIRECTION_CLASS_COUNT, DIRECTION_TRACK_COUNT, 3
            )

    model = MultiAccdoaDirectionNet()
    parameter_count = sum(parameter.numel() for parameter in model.parameters())
    if parameter_count > 2_000_000:
        raise RuntimeError(f"direction model exceeds the two-million-parameter budget: {parameter_count}")
    return model


def classwise_adpit_mse(predictions, targets, sample_weights=None):
    """Class-wise permutation-invariant Multi-ACCDOA MSE.

    Every class independently chooses an auxiliary-duplicated target: one
    source is copied to all tracks, one of two sources is duplicated, and three
    sources use ordinary permutations. This is the class-wise ADPIT target
    family used to prevent a semantic track order.
    """
    torch = require_torch()
    if predictions.shape != targets.shape or predictions.ndim != 4 or tuple(predictions.shape[1:]) != DIRECTION_OUTPUT_SHAPE:
        raise ValueError("ADPIT tensors must have shape [batch, 2, 3, 3]")
    per_scene_losses = []
    for batch_index in range(predictions.shape[0]):
        class_losses = []
        for class_index in range(DIRECTION_CLASS_COUNT):
            canonical = targets[batch_index, class_index]
            active = canonical[torch.linalg.vector_norm(canonical, dim=1) > 1.0e-6]
            active_count = int(active.shape[0])
            assignments = []
            if active_count == 0:
                assignments.append(torch.zeros_like(canonical))
            elif active_count == 1:
                assignments.append(active[0].unsqueeze(0).repeat(DIRECTION_TRACK_COUNT, 1))
            elif active_count == 2:
                index_assignments = set(itertools.permutations((0, 0, 1)))
                index_assignments.update(itertools.permutations((0, 1, 1)))
                for assignment in sorted(index_assignments):
                    assignments.append(torch.stack([active[index] for index in assignment]))
            elif active_count == 3:
                for permutation in _PERMUTATIONS:
                    assignments.append(torch.stack([active[index] for index in permutation]))
            else:
                raise ValueError("ADPIT supports no more than three active sources per class")
            candidate_targets = torch.stack(assignments)
            candidate_losses = (
                predictions[batch_index, class_index].unsqueeze(0) - candidate_targets
            ).square().mean(dim=(1, 2))
            class_losses.append(candidate_losses.min())
        per_scene_losses.append(torch.stack(class_losses).mean())
    per_scene = torch.stack(per_scene_losses)
    if sample_weights is not None:
        weights = sample_weights.to(device=per_scene.device, dtype=per_scene.dtype)
        if weights.shape != per_scene.shape:
            raise ValueError("ADPIT sample weights must have shape [batch]")
        return (per_scene * weights).sum() / weights.sum().clamp_min(1.0e-12)
    return per_scene.mean()


def _angular_error_vectors(left: np.ndarray, right: np.ndarray) -> float:
    left_norm = float(np.linalg.norm(left))
    right_norm = float(np.linalg.norm(right))
    if left_norm <= 1.0e-9 or right_norm <= 1.0e-9:
        return 180.0
    cosine = float(np.dot(left, right) / (left_norm * right_norm))
    return math.degrees(math.acos(float(np.clip(cosine, -1.0, 1.0))))


def decode_multi_accdoa(
    vectors: np.ndarray,
    thresholds: Sequence[float],
    enabled_classes: Sequence[bool] = (True, True),
    duplicate_merge_degrees: float = 7.5,
    maximum_sources: int = 3,
    elevation_bounds: tuple[float, float] = (-60.0, 60.0),
) -> list[PredictedDirection]:
    values = np.asarray(vectors, dtype=np.float32)
    if values.shape != DIRECTION_OUTPUT_SHAPE:
        raise ValueError("Multi-ACCDOA output must have shape [2, 3, 3]")
    if len(thresholds) != DIRECTION_CLASS_COUNT or len(enabled_classes) != DIRECTION_CLASS_COUNT:
        raise ValueError("direction thresholds/enabled mask must follow class order")
    if elevation_bounds[0] >= elevation_bounds[1]:
        raise ValueError("direction elevation bounds are invalid")
    candidates: list[tuple[PredictedDirection, np.ndarray]] = []
    for class_index, name in enumerate(CLASS_NAMES):
        if not enabled_classes[class_index]:
            continue
        for track_index in range(DIRECTION_TRACK_COUNT):
            vector = values[class_index, track_index]
            azimuth, elevation, confidence = vector_direction(vector)
            elevation = float(np.clip(elevation, *elevation_bounds))
            if confidence < float(thresholds[class_index]):
                continue
            prediction = PredictedDirection(name, azimuth, elevation, confidence, track_index)
            duplicate_index = next((
                index for index, (old, old_vector) in enumerate(candidates)
                if old.sound_class == name and _angular_error_vectors(vector, old_vector) < duplicate_merge_degrees
            ), None)
            if duplicate_index is None:
                candidates.append((prediction, vector.copy()))
            else:
                old, old_vector = candidates[duplicate_index]
                combined = old_vector * old.confidence + vector * confidence
                azimuth, elevation, _ = vector_direction(combined)
                elevation = float(np.clip(elevation, *elevation_bounds))
                merged = PredictedDirection(
                    name, azimuth, elevation, max(old.confidence, confidence), old.track_index
                )
                candidates[duplicate_index] = (merged, combined)
    return [candidate for candidate, _ in sorted(
        candidates, key=lambda item: item[0].confidence, reverse=True
    )[:maximum_sources]]


def _best_class_matches(
    truth: list[tuple[str, np.ndarray]], predictions: list[PredictedDirection]
) -> list[tuple[float, float, float]]:
    matches: list[tuple[float, float, float]] = []
    for name in CLASS_NAMES:
        truth_vectors = [vector for target_name, vector in truth if target_name == name]
        predicted = [item for item in predictions if item.sound_class == name]
        if not truth_vectors or not predicted:
            continue
        pairs: list[tuple[int, int]] = []
        if len(predicted) <= len(truth_vectors):
            best: tuple[float, tuple[int, ...]] | None = None
            for selection in itertools.permutations(range(len(truth_vectors)), len(predicted)):
                candidate_errors = [
                    _angular_error_vectors(
                        direction_vector(predicted[index].azimuth_degrees,
                                         predicted[index].elevation_degrees),
                        truth_vectors[truth_index],
                    )
                    for index, truth_index in enumerate(selection)
                ]
                score = sum(candidate_errors)
                if best is None or score < best[0]:
                    best = (score, tuple(selection))
            assert best is not None
            pairs = [(prediction_index, truth_index)
                     for prediction_index, truth_index in enumerate(best[1])]
        else:
            best_prediction_selection: tuple[int, ...] | None = None
            best_score = float("inf")
            for selection in itertools.permutations(range(len(predicted)), len(truth_vectors)):
                candidate_errors = [
                    _angular_error_vectors(
                        direction_vector(predicted[prediction_index].azimuth_degrees,
                                         predicted[prediction_index].elevation_degrees),
                        truth_vectors[index],
                    )
                    for index, prediction_index in enumerate(selection)
                ]
                if sum(candidate_errors) < best_score:
                    best_score = sum(candidate_errors)
                    best_prediction_selection = tuple(selection)
            assert best_prediction_selection is not None
            pairs = [(prediction_index, truth_index)
                     for truth_index, prediction_index in enumerate(best_prediction_selection)]
        for prediction_index, truth_index in pairs:
            prediction = predicted[prediction_index]
            truth_azimuth, truth_elevation, _ = vector_direction(truth_vectors[truth_index])
            del truth_azimuth
            error = _angular_error_vectors(
                direction_vector(prediction.azimuth_degrees, prediction.elevation_degrees),
                truth_vectors[truth_index],
            )
            matches.append((
                error,
                abs(prediction.elevation_degrees - truth_elevation),
                prediction.confidence,
            ))
    return matches


def _matched_count_within(
    truth: list[tuple[str, np.ndarray]],
    predictions: list[PredictedDirection],
    maximum_error_degrees: float,
) -> int:
    """Maximum class-wise bipartite match count within one angular cutoff."""
    total = 0
    for name in CLASS_NAMES:
        truth_vectors = [vector for target_name, vector in truth if target_name == name]
        predicted = [item for item in predictions if item.sound_class == name]
        if not truth_vectors or not predicted:
            continue
        errors = np.asarray([
            [
                _angular_error_vectors(
                    direction_vector(item.azimuth_degrees, item.elevation_degrees), vector
                )
                for vector in truth_vectors
            ]
            for item in predicted
        ], dtype=np.float64)
        best = 0
        if len(predicted) <= len(truth_vectors):
            for selection in itertools.permutations(range(len(truth_vectors)), len(predicted)):
                best = max(best, sum(
                    int(errors[prediction_index, truth_index] <= maximum_error_degrees)
                    for prediction_index, truth_index in enumerate(selection)
                ))
        else:
            for selection in itertools.permutations(range(len(predicted)), len(truth_vectors)):
                best = max(best, sum(
                    int(errors[prediction_index, truth_index] <= maximum_error_degrees)
                    for truth_index, prediction_index in enumerate(selection)
                ))
        total += best
    return total


def calibrate_direction_policy(outputs: np.ndarray, targets: np.ndarray) -> tuple[list[float], list[dict[str, float]]]:
    thresholds: list[float] = []
    for class_index in range(DIRECTION_CLASS_COUNT):
        best = (float("-inf"), 0.5)
        for threshold in np.linspace(0.10, 0.90, 33):
            true_positive = false_positive = false_negative = 0
            for scene_index in range(len(outputs)):
                truth_count = int(np.sum(np.linalg.norm(targets[scene_index, class_index], axis=1) > 0.5))
                predicted = decode_multi_accdoa(
                    outputs[scene_index],
                    [threshold] * DIRECTION_CLASS_COUNT,
                    enabled_classes=[index == class_index for index in range(DIRECTION_CLASS_COUNT)],
                )
                predicted_count = sum(item.sound_class == CLASS_NAMES[class_index] for item in predicted)
                true_positive += min(truth_count, predicted_count)
                false_positive += max(0, predicted_count - truth_count)
                false_negative += max(0, truth_count - predicted_count)
            precision = true_positive / max(1, true_positive + false_positive)
            recall = true_positive / max(1, true_positive + false_negative)
            f1 = 2.0 * precision * recall / max(1.0e-12, precision + recall)
            score = f1 - 0.02 * abs(float(threshold) - 0.5)
            if score > best[0]:
                best = (score, float(threshold))
        thresholds.append(best[1])

    confidence_edges = np.linspace(0.0, 1.0, 6)
    calibration: list[dict[str, float]] = []
    matched: list[tuple[float, float]] = []
    for scene_index in range(len(outputs)):
        truth = [
            (CLASS_NAMES[class_index], targets[scene_index, class_index, track])
            for class_index in range(DIRECTION_CLASS_COUNT)
            for track in range(DIRECTION_TRACK_COUNT)
            if np.linalg.norm(targets[scene_index, class_index, track]) > 0.5
        ]
        predictions = decode_multi_accdoa(outputs[scene_index], thresholds)
        # Greedy confidence/error pairs are adequate for monotonic HUD calibration;
        # the gate metrics below use exact small-set assignment.
        remaining = list(truth)
        for prediction in predictions:
            candidates = [
                (_angular_error_vectors(
                    direction_vector(prediction.azimuth_degrees, prediction.elevation_degrees), vector
                ), index)
                for index, (name, vector) in enumerate(remaining) if name == prediction.sound_class
            ]
            if not candidates:
                continue
            error, index = min(candidates)
            remaining.pop(index)
            matched.append((prediction.confidence, error))
    for edge in confidence_edges:
        errors = [error for confidence, error in matched if confidence >= edge]
        uncertainty = float(np.percentile(errors, 90)) if errors else 180.0
        calibration.append({"confidence": float(edge), "p90_angular_error_degrees": uncertainty})
    # Confidence should never make the displayed p90 interval wider.
    for index in range(1, len(calibration)):
        calibration[index]["p90_angular_error_degrees"] = min(
            calibration[index - 1]["p90_angular_error_degrees"],
            calibration[index]["p90_angular_error_degrees"],
        )
    return thresholds, calibration


class _DirectionCacheDataset:
    def __init__(self, cache_dir: Path, split: str):
        _cache_metadata(cache_dir)
        root = cache_dir / split
        self.features = np.load(root / "features.npy", mmap_mode="r", allow_pickle=False)
        self.targets = np.load(root / "targets.npy", mmap_mode="r", allow_pickle=False)
        self.counts = np.load(root / "counts.npy", allow_pickle=False)
        self.close = np.load(root / "close.npy", allow_pickle=False)
        if (not len(self.features)
                or not (len(self.features) == len(self.targets) == len(self.counts) == len(self.close))):
            raise ValueError("direction cache arrays have inconsistent lengths")
        if (self.features.shape[1:] != (
                INPUT_CHANNELS, DIRECTION_SCENE_FRAMES, MEL_BINS)
                or self.targets.shape[1:] != DIRECTION_OUTPUT_SHAPE
                or self.counts.ndim != 1 or self.close.ndim != 1
                or np.any(self.counts > DIRECTION_TRACK_COUNT)):
            raise ValueError("direction cache arrays have incompatible shapes or counts")

    def __len__(self) -> int:
        return len(self.features)

    def __getitem__(self, index: int) -> tuple[np.ndarray, np.ndarray, int, bool]:
        return (
            np.array(self.features[index], dtype=np.float32, copy=True),
            np.array(self.targets[index], dtype=np.float32, copy=True),
            int(self.counts[index]), bool(self.close[index]),
        )


def _predict_dataset(model, dataset: _DirectionCacheDataset, torch, batch_size: int) -> np.ndarray:
    output: list[np.ndarray] = []
    model.eval()
    with torch.no_grad():
        for start in range(0, len(dataset), batch_size):
            values = np.stack([dataset[index][0] for index in range(start, min(len(dataset), start + batch_size))])
            output.append(model(torch.from_numpy(values)).cpu().numpy())
    return np.concatenate(output, axis=0)


def train_direction_model(
    cache_dir: str | Path,
    output_dir: str | Path,
    epochs: int = 20,
    seed: int = 20260720,
    batch_size: int = 64,
    threads: int = 0,
    resume: str | Path | None = None,
) -> Path:
    torch = require_torch()
    if epochs <= 0 or batch_size <= 0 or threads < 0:
        raise ValueError("direction epochs/batch size must be positive and threads non-negative")
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.use_deterministic_algorithms(True)
    if threads > 0:
        torch.set_num_threads(threads)

    cache = Path(cache_dir)
    metadata = _cache_metadata(cache)
    train_data = _DirectionCacheDataset(cache, "train")
    dev_data = _DirectionCacheDataset(cache, "dev")
    model = build_direction_model()
    parameter_count = sum(parameter.numel() for parameter in model.parameters())
    optimizer = torch.optim.AdamW(model.parameters(), lr=8.0e-4, weight_decay=1.0e-4)
    start_epoch = 0
    if resume:
        checkpoint = torch.load(resume, map_location="cpu", weights_only=False)
        if checkpoint.get("preprocessing_version") != DIRECTION_PREPROCESSING_VERSION:
            raise ValueError("direction resume checkpoint preprocessing is incompatible")
        if checkpoint.get("manifest_sha256") != metadata["manifest_sha256"]:
            raise ValueError("direction resume checkpoint was trained from a different manifest")
        if int(checkpoint.get("seed", -1)) != seed:
            raise ValueError("direction resume checkpoint uses a different deterministic seed")
        if int(checkpoint.get("parameter_count", -1)) != parameter_count:
            raise ValueError("direction resume checkpoint uses a different model architecture")
        model.load_state_dict(checkpoint["model"])
        optimizer.load_state_dict(checkpoint["optimizer"])
        start_epoch = int(checkpoint["epoch"])
        if start_epoch > epochs:
            raise ValueError("direction resume checkpoint is newer than requested epochs")
        if "torch_rng_state" in checkpoint:
            torch.set_rng_state(checkpoint["torch_rng_state"])
        if "numpy_rng_state" in checkpoint:
            np.random.set_state(checkpoint["numpy_rng_state"])
        if "python_rng_state" in checkpoint:
            random.setstate(checkpoint["python_rng_state"])

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    base_weights = np.asarray([0.75, 1.0, 1.45, 1.75], dtype=np.float64)[train_data.counts]
    base_weights *= np.where(train_data.close, 1.75, 1.0)
    for epoch in range(start_epoch, epochs):
        generator = torch.Generator().manual_seed(seed + epoch)
        sampler = torch.utils.data.WeightedRandomSampler(
            torch.from_numpy(base_weights), len(train_data), replacement=True, generator=generator
        )
        loader = torch.utils.data.DataLoader(
            train_data, batch_size=batch_size, sampler=sampler, num_workers=0, drop_last=False
        )
        model.train()
        losses: list[float] = []
        augmentation_rng = torch.Generator().manual_seed(seed ^ (epoch + 1) * 65537)
        for values, targets, _counts, _close in loader:
            values = values.float()
            targets = targets.float()
            mirror = torch.rand(values.shape[0], generator=augmentation_rng) < 0.5
            if bool(mirror.any()):
                values[mirror, 2] *= -1.0
                values[mirror, 4] *= -1.0
                targets[mirror, :, :, 0] *= -1.0
            optimizer.zero_grad(set_to_none=True)
            prediction = model(values)
            loss = classwise_adpit_mse(prediction, targets)
            if not bool(torch.isfinite(loss)):
                raise RuntimeError("direction training produced a non-finite ADPIT loss")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            optimizer.step()
            losses.append(float(loss.detach()))
        checkpoint_path = output / "direction-checkpoint.pt"
        checkpoint_partial = output / "direction-checkpoint.pt.partial"
        torch.save({
            "epoch": epoch + 1, "model": model.state_dict(), "optimizer": optimizer.state_dict(),
            "seed": seed, "preprocessing_version": DIRECTION_PREPROCESSING_VERSION,
            "manifest_sha256": metadata["manifest_sha256"], "parameter_count": parameter_count,
            "torch_rng_state": torch.get_rng_state(),
            "numpy_rng_state": np.random.get_state(),
            "python_rng_state": random.getstate(),
        }, checkpoint_partial)
        checkpoint_partial.replace(checkpoint_path)
        print(f"direction epoch {epoch + 1}/{epochs} loss={np.mean(losses):.6f}", flush=True)

    dev_output = _predict_dataset(model, dev_data, torch, batch_size)
    if not np.isfinite(dev_output).all():
        raise RuntimeError("direction model produced non-finite development outputs")
    dev_targets = np.asarray(dev_data.targets, dtype=np.float32)
    thresholds, uncertainty = calibrate_direction_policy(dev_output, dev_targets)

    class ExportedDirectionModel(torch.nn.Module):
        def __init__(self, inner):
            super().__init__()
            self.inner = inner

        def forward(self, features):
            return self.inner(features)

    exported = ExportedDirectionModel(model).eval()
    dummy = torch.zeros((1, INPUT_CHANNELS, DIRECTION_SCENE_FRAMES, MEL_BINS), dtype=torch.float32)
    dummy[:, 1] = -100.0
    model_path = output / "direction.onnx"
    torch.onnx.export(
        exported, dummy, model_path, input_names=["features"], output_names=["multi_accdoa"],
        dynamic_axes=None, opset_version=17, dynamo=False,
    )
    digest = hashlib.sha256(model_path.read_bytes()).hexdigest()
    package_metadata: dict[str, object] = {
        "package_version": DIRECTION_PACKAGE_VERSION,
        "model_version": f"cs2-direction-v1-{seed}",
        "model_file": model_path.name,
        "model_sha256": digest,
        "preprocessing_version": DIRECTION_PREPROCESSING_VERSION,
        "sample_rate": SAMPLE_RATE,
        "fft_size": FFT_SIZE,
        "hop_size": HOP_SIZE,
        "mel_bins": MEL_BINS,
        "context_frames": DIRECTION_SCENE_FRAMES,
        "context_samples": DIRECTION_SCENE_SAMPLES,
        "input_channels": INPUT_CHANNELS,
        "class_order": ",".join(CLASS_NAMES),
        "track_order": "exchangeable-0,exchangeable-1,exchangeable-2",
        "track_count": DIRECTION_TRACK_COUNT,
        "coordinate_system": "x-right,y-up,z-forward",
        "elevation_min_degrees": -60.0,
        "elevation_max_degrees": 60.0,
        "threshold_gunshot": thresholds[CLASS_NAMES.index("gunshot")],
        "threshold_footstep": thresholds[CLASS_NAMES.index("footstep")],
        "duplicate_merge_degrees": 7.5,
        "minimum_training_separation_degrees": MINIMUM_SOURCE_SEPARATION_DEGREES,
        "maximum_sources": 3,
        "uncertainty_count": len(uncertainty),
        "pcen_smoothing": PCEN_SMOOTHING,
        "pcen_alpha": PCEN_ALPHA,
        "pcen_delta": PCEN_DELTA,
        "pcen_root": PCEN_ROOT,
        "pcen_epsilon": PCEN_EPSILON,
    }
    for index, entry in enumerate(uncertainty):
        package_metadata[f"uncertainty_confidence_{index}"] = entry["confidence"]
        package_metadata[f"uncertainty_p90_degrees_{index}"] = entry["p90_angular_error_degrees"]
    (output / "direction.json").write_text(
        json.dumps(package_metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    with torch.no_grad():
        expected = exported(dummy).cpu().numpy()
    np.savez_compressed(output / "direction-parity.npz", input=dummy.numpy(), torch_output=expected)
    summary = {
        "seed": seed, "epochs": epochs, "batch_size": batch_size,
        "parameter_count": parameter_count, "train_scenes": len(train_data),
        "development_scenes": len(dev_data), "manifest_sha256": metadata["manifest_sha256"],
        "activity_thresholds": dict(zip(CLASS_NAMES, thresholds, strict=True)),
        "uncertainty_calibration": uncertainty,
        "weighted_sampling": {"two_source": 1.45, "three_source": 1.75, "close_15_30": 1.75},
    }
    (output / "training-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return output


def load_direction_package(directory: str | Path) -> tuple[dict, Path]:
    root = Path(directory)
    metadata_path = root / "direction.json"
    if not metadata_path.is_file():
        raise ValueError(f"missing direction package metadata: {metadata_path}")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    required = {
        "package_version": DIRECTION_PACKAGE_VERSION,
        "preprocessing_version": DIRECTION_PREPROCESSING_VERSION,
        "sample_rate": SAMPLE_RATE,
        "fft_size": FFT_SIZE,
        "hop_size": HOP_SIZE,
        "mel_bins": MEL_BINS,
        "context_frames": DIRECTION_SCENE_FRAMES,
        "context_samples": DIRECTION_SCENE_SAMPLES,
        "input_channels": INPUT_CHANNELS,
        "class_order": ",".join(CLASS_NAMES),
        "track_order": "exchangeable-0,exchangeable-1,exchangeable-2",
        "track_count": DIRECTION_TRACK_COUNT,
        "coordinate_system": "x-right,y-up,z-forward",
        "maximum_sources": 3,
        "minimum_training_separation_degrees": MINIMUM_SOURCE_SEPARATION_DEGREES,
        "pcen_smoothing": PCEN_SMOOTHING,
        "pcen_alpha": PCEN_ALPHA,
        "pcen_delta": PCEN_DELTA,
        "pcen_root": PCEN_ROOT,
        "pcen_epsilon": PCEN_EPSILON,
    }
    for key, expected in required.items():
        if metadata.get(key) != expected:
            raise ValueError(f"incompatible direction package {key}: {metadata.get(key)!r}")
    if not str(metadata.get("model_version", "")):
        raise ValueError("direction package model version is missing")
    thresholds = [float(metadata.get(f"threshold_{name}", 0.0)) for name in CLASS_NAMES]
    if any(not 0.0 < value <= 1.0 for value in thresholds):
        raise ValueError("direction package activity thresholds are invalid")
    if (float(metadata.get("elevation_min_degrees", 0.0)),
        float(metadata.get("elevation_max_degrees", 0.0))) != (-60.0, 60.0):
        raise ValueError("direction package elevation bounds are incompatible")
    if not 0.0 < float(metadata.get("duplicate_merge_degrees", 0.0)) < 15.0:
        raise ValueError("direction package duplicate-merge policy is invalid")
    if float(metadata.get("minimum_training_separation_degrees", 0.0)) != 15.0:
        raise ValueError("direction package training-separation contract is invalid")
    uncertainty_count = int(metadata.get("uncertainty_count", 0))
    if not 2 <= uncertainty_count <= 16:
        raise ValueError("direction package uncertainty calibration count is invalid")
    previous_confidence = -1.0
    previous_error = 181.0
    for index in range(uncertainty_count):
        confidence = float(metadata.get(f"uncertainty_confidence_{index}", -1.0))
        angular_error = float(metadata.get(f"uncertainty_p90_degrees_{index}", -1.0))
        if (not 0.0 <= confidence <= 1.0 or confidence <= previous_confidence
                or not 0.0 <= angular_error <= 180.0 or angular_error > previous_error + 1.0e-6):
            raise ValueError("direction package uncertainty calibration is invalid")
        previous_confidence = confidence
        previous_error = angular_error
    model_file = Path(str(metadata.get("model_file", "")))
    if not model_file.name or model_file.is_absolute() or model_file.parent != Path("."):
        raise ValueError("direction model file must be inside its package directory")
    model_path = root / model_file
    if not model_path.is_file():
        raise ValueError(f"missing direction ONNX model: {model_path}")
    if hashlib.sha256(model_path.read_bytes()).hexdigest() != metadata.get("model_sha256"):
        raise ValueError("direction ONNX model SHA-256 does not match direction.json")
    return metadata, model_path


def check_direction_parity(package_dir: str | Path, fixture: str | Path | None = None) -> float:
    try:
        import onnxruntime
    except ImportError as error:
        raise RuntimeError("direction parity requires ONNX Runtime") from error
    metadata, model_path = load_direction_package(package_dir)
    fixture_path = Path(fixture) if fixture else Path(package_dir) / "direction-parity.npz"
    values = np.load(fixture_path, allow_pickle=False)
    input_values = np.asarray(values["input"], dtype=np.float32)
    expected = np.asarray(values["torch_output"], dtype=np.float32)
    options = onnxruntime.SessionOptions()
    options.intra_op_num_threads = 1
    options.graph_optimization_level = onnxruntime.GraphOptimizationLevel.ORT_ENABLE_ALL
    session = onnxruntime.InferenceSession(
        str(model_path), sess_options=options, providers=["CPUExecutionProvider"]
    )
    actual = session.run(["multi_accdoa"], {"features": input_values})[0]
    if (actual.shape != expected.shape or actual.shape[1:] != DIRECTION_OUTPUT_SHAPE
            or not np.isfinite(actual).all() or not np.isfinite(expected).all()):
        raise ValueError("direction ONNX parity output shape is incompatible")
    maximum = float(np.max(np.abs(actual - expected)))
    if maximum > 1.0e-5:
        raise ValueError(f"direction PyTorch/ONNX parity exceeded 1e-5: {maximum}")
    del metadata
    return maximum


def _truth_from_row(row: dict) -> list[tuple[str, np.ndarray]]:
    return [
        (
            target["sound_class"],
            direction_vector(
                float(target["rendered_direction"]["azimuth_degrees"]),
                float(target["rendered_direction"]["elevation_degrees"]),
            ),
        )
        for target in row["targets"]
    ]


def evaluate_direction_outputs(
    rows: Sequence[dict],
    outputs: np.ndarray,
    thresholds: Sequence[float],
    footstep_only: bool = False,
    gate: str = "synthetic",
    inference_milliseconds: Sequence[float] = (),
    processing_milliseconds: Sequence[float] = (),
) -> DirectionMetrics:
    if len(rows) != len(outputs):
        raise ValueError("direction rows and outputs have different lengths")
    if gate not in ("synthetic", "real"):
        raise ValueError("direction evaluation gate must be synthetic or real")
    exact: list[bool] = []
    exact_two: list[bool] = []
    exact_three: list[bool] = []
    close_exact: list[bool] = []
    matched_errors: list[float] = []
    elevation_errors: list[float] = []
    close_errors: list[float] = []
    true_positive_20 = true_positive_30 = false_positive_20 = false_positive_30 = 0
    false_negative_20 = false_negative_30 = 0
    high_confidence_total = high_confidence_catastrophic = 0
    disabled_gunshots = total_predictions = 0
    enabled = (not footstep_only, True)
    for row, vectors in zip(rows, outputs, strict=True):
        truth = _truth_from_row(row)
        if footstep_only:
            truth = [item for item in truth if item[0] != "gunshot"]
        predictions = decode_multi_accdoa(vectors, thresholds, enabled_classes=enabled)
        if footstep_only:
            disabled_gunshots += sum(item.sound_class == "gunshot" for item in predictions)
        total_predictions += len(predictions)
        is_exact = len(predictions) == len(truth)
        exact.append(is_exact)
        if int(row["target_count"]) == 2:
            exact_two.append(is_exact)
        if int(row["target_count"]) == 3:
            exact_three.append(is_exact)
        if row.get("stratum") == "close-15-30":
            close_exact.append(is_exact)
        matches = _best_class_matches(truth, predictions)
        errors = [match[0] for match in matches]
        matched_errors.extend(errors)
        elevation_errors.extend(match[1] for match in matches)
        if row.get("stratum") == "close-15-30":
            close_errors.extend(errors)
        within_20 = _matched_count_within(truth, predictions, 20.0)
        within_30 = _matched_count_within(truth, predictions, 30.0)
        true_positive_20 += within_20
        false_positive_20 += len(predictions) - within_20
        false_negative_20 += len(truth) - within_20
        true_positive_30 += within_30
        false_positive_30 += len(predictions) - within_30
        false_negative_30 += len(truth) - within_30
        for error, _, confidence in matches:
            if confidence >= 0.8:
                high_confidence_total += 1
                high_confidence_catastrophic += int(error > 45.0)
    precision = true_positive_20 / max(1, true_positive_20 + false_positive_20)
    recall = true_positive_20 / max(1, true_positive_20 + false_negative_20)
    f1 = 2.0 * precision * recall / max(1.0e-12, precision + recall)
    precision_30 = true_positive_30 / max(1, true_positive_30 + false_positive_30)
    recall_30 = true_positive_30 / max(1, true_positive_30 + false_negative_30)
    f1_30 = 2.0 * precision_30 * recall_30 / max(1.0e-12, precision_30 + recall_30)
    median = float(np.median(matched_errors)) if matched_errors else 180.0
    p90 = float(np.percentile(matched_errors, 90)) if matched_errors else 180.0
    elevation_p90 = float(np.percentile(elevation_errors, 90)) if elevation_errors else 180.0
    close_p90 = float(np.percentile(close_errors, 90)) if close_errors else 180.0
    exact_accuracy = float(np.mean(exact)) if exact else 0.0
    exact_two_accuracy = float(np.mean(exact_two)) if exact_two else 0.0
    exact_three_accuracy = float(np.mean(exact_three)) if exact_three else 0.0
    close_accuracy = float(np.mean(close_exact)) if close_exact else 0.0
    catastrophic = high_confidence_catastrophic / max(1, high_confidence_total)
    disabled_rate = disabled_gunshots / max(1, total_predictions)
    p95_inference = (
        float(np.percentile(inference_milliseconds, 95))
        if len(inference_milliseconds) else 0.0
    )
    p95_processing = (
        float(np.percentile(processing_milliseconds, 95))
        if len(processing_milliseconds) else p95_inference
    )
    delivery_milliseconds = (
        DIRECTION_SCENE_MILLISECONDS - DIRECTION_PRE_ANCHOR_MILLISECONDS + p95_processing
    )
    rooms = {str(row.get("room", "")) for row in rows if row.get("room")}
    capture_sessions = {
        str(row.get("capture_session", "")) for row in rows if row.get("capture_session")
    }
    elevation_bands = set()
    profiles = {
        json.dumps(row["profile"], sort_keys=True, separators=(",", ":"))
        for row in rows if row.get("profile")
    }
    for row in rows:
        for target in row["targets"]:
            elevation = float(target["rendered_direction"]["elevation_degrees"])
            elevation_bands.add("down" if elevation < -20.0 else "up" if elevation > 20.0 else "level")
    if gate == "synthetic":
        acceptance = (
            exact_accuracy >= 0.90 and exact_two_accuracy >= 0.85 and exact_three_accuracy >= 0.85
            and precision >= 0.90 and recall >= 0.90 and f1 >= 0.90
            and median <= 10.0 and p90 <= 20.0
            and close_accuracy >= 0.75 and close_p90 <= 15.0
            and catastrophic < 0.01 and disabled_rate < 0.02
        )
    else:
        acceptance = (
            len(rows) >= 1200 and len(exact_two) >= 200 and len(exact_three) >= 200
            and len(rooms) >= 3 and len(capture_sessions) >= 3 and len(elevation_bands) >= 3
            and len(profiles) == 1
            and exact_accuracy >= 0.80 and exact_two_accuracy >= 0.75 and exact_three_accuracy >= 0.75
            and f1_30 >= 0.80 and median <= 15.0 and p90 <= 30.0
            and elevation_p90 <= 25.0 and disabled_rate < 0.05
            and p95_inference <= 15.0
            and delivery_milliseconds <= 300.0
        )
    return DirectionMetrics(
        gate=gate, scene_count=len(rows), two_source_support=len(exact_two),
        three_source_support=len(exact_three), real_room_count=len(rooms),
        real_capture_session_count=len(capture_sessions),
        real_elevation_band_count=len(elevation_bands),
        real_profile_count=len(profiles),
        exact_count_accuracy=exact_accuracy,
        exact_count_accuracy_two_source=exact_two_accuracy,
        exact_count_accuracy_three_source=exact_three_accuracy,
        source_precision_within_20_degrees=precision,
        source_recall_within_20_degrees=recall,
        source_f1_within_20_degrees=f1,
        source_precision_within_30_degrees=precision_30,
        source_recall_within_30_degrees=recall_30,
        source_f1_within_30_degrees=f1_30,
        median_matched_error_degrees=median, p90_matched_error_degrees=p90,
        p90_elevation_error_degrees=elevation_p90,
        close_exact_count_accuracy=close_accuracy,
        close_p90_matched_error_degrees=close_p90,
        high_confidence_error_over_45_rate=catastrophic,
        disabled_gunshot_inclusion_rate=disabled_rate,
        p95_inference_milliseconds=p95_inference,
        scene_window_delivery_milliseconds=delivery_milliseconds,
        acceptance_passed=acceptance,
    )


def evaluate_direction_package(
    manifest_path: str | Path,
    audio_root: str | Path,
    package_dir: str | Path,
    split: str = "test",
    footstep_only: bool = False,
    gate: str = "auto",
) -> DirectionMetrics:
    try:
        import onnxruntime
    except ImportError as error:
        raise RuntimeError("direction evaluation requires ONNX Runtime") from error
    metadata, model_path = load_direction_package(package_dir)
    all_rows = load_direction_scene_rows(manifest_path, allow_real=True)
    rows = [row for row in all_rows if row["split"] == split]
    if not rows:
        raise ValueError(f"direction manifest contains no {split} scenes")
    options = onnxruntime.SessionOptions()
    options.intra_op_num_threads = 1
    options.graph_optimization_level = onnxruntime.GraphOptimizationLevel.ORT_ENABLE_ALL
    session = onnxruntime.InferenceSession(
        str(model_path), sess_options=options, providers=["CPUExecutionProvider"]
    )
    warm_input = np.zeros(
        (1, INPUT_CHANNELS, DIRECTION_SCENE_FRAMES, MEL_BINS), dtype=np.float32
    )
    warm_input[:, 1] = -100.0
    warm_output = session.run(["multi_accdoa"], {"features": warm_input})[0]
    if warm_output.shape != (1, *DIRECTION_OUTPUT_SHAPE) or not np.isfinite(warm_output).all():
        raise ValueError("direction ONNX startup output must be finite [1,2,3,3]")
    outputs: list[np.ndarray] = []
    inference_times: list[float] = []
    processing_times: list[float] = []
    root = Path(audio_root)
    for row in rows:
        wav_path = root / row["relative_path"]
        expected_hash = str(row.get("wav_sha256", ""))
        if expected_hash and hashlib.sha256(wav_path.read_bytes()).hexdigest() != expected_hash:
            raise ValueError(f"direction evaluation WAV hash mismatch: {wav_path}")
        audio = to_stereo_48k(load_pcm_wav(wav_path))
        if len(audio.samples) != DIRECTION_SCENE_SAMPLES:
            raise ValueError(f"direction evaluation scene has an invalid length: {wav_path}")
        processing_started = time.perf_counter()
        features = np.ascontiguousarray(
            stereo_onset_features(audio.samples).transpose(1, 0, 2)[None],
            dtype=np.float32,
        )
        started = time.perf_counter()
        batch_output = session.run(["multi_accdoa"], {"features": features})[0]
        inference_times.append((time.perf_counter() - started) * 1000.0)
        processing_times.append((time.perf_counter() - processing_started) * 1000.0)
        if batch_output.shape != (1, *DIRECTION_OUTPUT_SHAPE) or not np.isfinite(batch_output).all():
            raise ValueError("direction ONNX scene output must be finite [1,2,3,3]")
        outputs.append(batch_output[0])
    thresholds = [float(metadata[f"threshold_{name}"]) for name in CLASS_NAMES]
    schemas = {row["schema_version"] for row in rows}
    if len(schemas) != 1:
        raise ValueError("direction evaluation cannot mix synthetic and real scene schemas")
    selected_gate = (
        "real" if schemas == {DIRECTION_REAL_SCENE_SCHEMA} else "synthetic"
    ) if gate == "auto" else gate
    return evaluate_direction_outputs(
        rows, np.stack(outputs), thresholds, footstep_only=footstep_only,
        gate=selected_gate, inference_milliseconds=inference_times,
        processing_milliseconds=processing_times,
    )
