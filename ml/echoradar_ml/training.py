from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import random
from typing import Iterable

import numpy as np

from . import (
    CLASS_NAMES, CONTEXT_FRAMES, FFT_SIZE, HOP_SIZE, INFERENCE_STRIDE_FRAMES,
    INPUT_CHANNELS, MEL_BINS, PCEN_ALPHA, PCEN_DELTA, PCEN_EPSILON, PCEN_ROOT,
    PCEN_SMOOTHING, PREPROCESSING_VERSION, SAMPLE_RATE, SOURCE_NAMES,
)
from .audio import load_pcm_wav, to_stereo_48k
from .features import scene_activity, stereo_onset_features
from .model import build_model, require_torch
from .sessions import load_timeline


ONSET_WEIGHTS = (1.0, 0.6, 0.2)
MIN_SPACING_MS = {"gunshot": 35, "footstep": 80}
PEAK_LOOKAHEAD_FRAMES = 2
MATCH_TOLERANCE_SAMPLES = SAMPLE_RATE // 10
CACHE_VERSION = "feature-cache-v4.1"


def _usable_events(path: Path) -> list[dict]:
    return [
        event for event in load_timeline(path)
        if event.get("class") in CLASS_NAMES
        and not bool(event.get("uncertain", False))
        and bool(event.get("reviewed", True))
    ]


def _targets(frame_count: int, events: list[dict]) -> np.ndarray:
    output = np.zeros((frame_count, len(CLASS_NAMES)), dtype=np.float32)
    for event in events:
        if event.get("class") not in CLASS_NAMES or bool(event.get("uncertain", False)):
            continue
        class_index = CLASS_NAMES.index(event["class"])
        onset = int(event["onset_sample"])
        # Frame i is causally available at i*hop + fft.  Supervise the first
        # frame with a full hop of post-onset evidence; the Hann endpoint itself
        # contains no onset energy.
        first_evidence_sample = onset + HOP_SIZE
        start = max(0, (first_evidence_sample - FFT_SIZE + HOP_SIZE - 1) // HOP_SIZE)
        for offset, weight in enumerate(ONSET_WEIGHTS):
            if start + offset < frame_count:
                output[start + offset, class_index] = max(output[start + offset, class_index], weight)
    return output


def _source_targets(frame_count: int, events: list[dict]) -> np.ndarray:
    output = np.full((frame_count, len(CLASS_NAMES)), -1, dtype=np.int64)
    for event in events:
        if event.get("class") not in CLASS_NAMES or bool(event.get("uncertain", False)):
            continue
        source = str(event.get("source_hint", "unknown"))
        if source not in SOURCE_NAMES:
            source = "unknown"
        class_index = CLASS_NAMES.index(event["class"])
        onset = int(event["onset_sample"])
        first_evidence_sample = onset + HOP_SIZE
        start = max(0, (first_evidence_sample - FFT_SIZE + HOP_SIZE - 1) // HOP_SIZE)
        for offset in range(len(ONSET_WEIGHTS)):
            if start + offset < frame_count:
                output[start + offset, class_index] = SOURCE_NAMES.index(source)
    return output


def _window_ends(labels: np.ndarray, stride_frames: int) -> list[int]:
    if stride_frames <= 0:
        raise ValueError("window stride must be positive")
    # Include startup windows: runtime left-pads them instead of waiting for a
    # complete context, and training must expose the model to that same state.
    ends = set(range(1, len(labels) + 1, stride_frames))
    ends.add(len(labels))
    for frame in np.flatnonzero(labels.max(axis=1) > 0.0):
        ends.add(int(frame) + 1)
    return sorted(end for end in ends if end > 0)


def padded_window(features: np.ndarray, end: int) -> np.ndarray:
    if features.ndim != 3 or features.shape[1:] != (INPUT_CHANNELS, MEL_BINS):
        raise ValueError("features must have shape [time, 5, 64]")
    if not (0 < end <= len(features)):
        raise ValueError("window endpoint is outside feature sequence")
    output = np.zeros((CONTEXT_FRAMES, INPUT_CHANNELS, MEL_BINS), dtype=np.float32)
    output[:, 1, :] = -100.0
    take = min(CONTEXT_FRAMES, end)
    output[-take:] = features[end - take:end]
    return output.transpose(1, 0, 2)


def load_windows(
    session_prefixes: list[Path], stride_frames: int = 20,
    gain_db_values: tuple[float, ...] = (0.0,),
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Small-corpus helper used by tests and smoke runs.

    Full training uses :class:`CachedWindowDataset` and never materializes all
    overlapping windows.
    """
    inputs: list[np.ndarray] = []
    targets: list[np.ndarray] = []
    sources: list[np.ndarray] = []
    for prefix in session_prefixes:
        audio = to_stereo_48k(load_pcm_wav(prefix.with_suffix(".wav")))
        events = _usable_events(prefix.with_suffix(".jsonl"))
        for gain_db in gain_db_values:
            features = stereo_onset_features(audio.samples, gain_db)
            labels = _targets(len(features), events)
            source_labels = _source_targets(len(features), events)
            for end in _window_ends(labels, stride_frames):
                inputs.append(padded_window(features, end))
                targets.append(labels[end - 1])
                sources.append(source_labels[end - 1])
    if not inputs:
        raise ValueError("no training windows were generated")
    return np.stack(inputs), np.stack(targets), np.stack(sources)


@dataclass(frozen=True)
class CacheRecord:
    prefix: Path
    gain_db: float
    domain: str
    features_path: Path
    labels_path: Path
    sources_path: Path
    activity_path: Path
    ends: tuple[int, ...]


def _cache_key(prefix: Path, gain_db: float) -> str:
    wav = prefix.with_suffix(".wav")
    labels = prefix.with_suffix(".jsonl")
    payload = {
        "prefix": str(prefix.resolve()),
        "wav_size": wav.stat().st_size,
        "wav_mtime": wav.stat().st_mtime_ns,
        "labels_size": labels.stat().st_size,
        "labels_mtime": labels.stat().st_mtime_ns,
        "preprocessing": PREPROCESSING_VERSION,
        "cache_version": CACHE_VERSION,
        "gain_db": gain_db,
    }
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()[:20]


def prepare_feature_cache(
    session_prefixes: Iterable[Path], cache_dir: str | Path,
    gain_db_values: tuple[float, ...], stride_frames: int = 20,
) -> list[CacheRecord]:
    root = Path(cache_dir)
    root.mkdir(parents=True, exist_ok=True)
    records: list[CacheRecord] = []
    prefixes = list(session_prefixes)
    for session_index, prefix in enumerate(prefixes):
        audio = to_stereo_48k(load_pcm_wav(prefix.with_suffix(".wav")))
        events = _usable_events(prefix.with_suffix(".jsonl"))
        session_metadata_path = prefix.with_suffix(".session.json")
        session_metadata = (
            json.loads(session_metadata_path.read_text(encoding="utf-8"))
            if session_metadata_path.exists() else {}
        )
        domain = "real" if (
            str(session_metadata.get("generator_version", "")).startswith("real-session")
            or "_real_" in prefix.name
        ) else "synthetic"
        for gain_db in gain_db_values:
            key = _cache_key(prefix, gain_db)
            paths = {
                "features": root / f"{key}.features.npy",
                "labels": root / f"{key}.labels.npy",
                "sources": root / f"{key}.sources.npy",
                "activity": root / f"{key}.activity.npy",
                "meta": root / f"{key}.json",
            }
            if not all(paths[name].exists() for name in paths):
                features = stereo_onset_features(audio.samples, gain_db)
                labels = _targets(len(features), events)
                sources = _source_targets(len(features), events)
                activity = scene_activity(features, round(0.5 * SAMPLE_RATE / HOP_SIZE))
                np.save(paths["features"], features, allow_pickle=False)
                np.save(paths["labels"], labels, allow_pickle=False)
                np.save(paths["sources"], sources, allow_pickle=False)
                np.save(paths["activity"], activity, allow_pickle=False)
                paths["meta"].write_text(json.dumps({
                    "prefix": str(prefix.resolve()), "gain_db": gain_db,
                    "frames": len(features), "domain": domain,
                    "cache_version": CACHE_VERSION,
                    "preprocessing_version": PREPROCESSING_VERSION,
                }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            labels_array = np.load(paths["labels"], mmap_mode="r", allow_pickle=False)
            records.append(CacheRecord(
                prefix=prefix, gain_db=gain_db, domain=domain, features_path=paths["features"],
                labels_path=paths["labels"], sources_path=paths["sources"],
                activity_path=paths["activity"],
                ends=tuple(_window_ends(labels_array, stride_frames)),
            ))
        print(f"feature cache {session_index + 1}/{len(prefixes)}: {prefix.name}", flush=True)
    return records


class CachedWindowDataset:
    def __init__(self, records: list[CacheRecord]):
        self.records = records
        self.index: list[tuple[int, int]] = []
        self.positive_indices: list[int] = []
        self.negative_indices: list[int] = []
        self.real_positive_indices: list[int] = []
        self.real_negative_indices: list[int] = []
        self.synthetic_positive_indices: list[int] = []
        self.synthetic_negative_indices: list[int] = []
        self.domain_by_index: list[str] = []
        self._arrays: dict[Path, np.ndarray] = {}
        for record_index, record in enumerate(records):
            labels = np.load(record.labels_path, mmap_mode="r", allow_pickle=False)
            for end in record.ends:
                dataset_index = len(self.index)
                self.index.append((record_index, end))
                self.domain_by_index.append(record.domain)
                target = labels[end - 1]
                positive = float(target.max()) > 0.0
                (self.positive_indices if positive else self.negative_indices).append(dataset_index)
                if record.domain == "real":
                    (self.real_positive_indices if positive else self.real_negative_indices).append(dataset_index)
                else:
                    (self.synthetic_positive_indices if positive else self.synthetic_negative_indices).append(dataset_index)
        if not self.positive_indices or not self.negative_indices:
            raise ValueError("training requires both onset-bearing and negative windows")

    def __len__(self) -> int:
        return len(self.index)

    def _load(self, path: Path) -> np.ndarray:
        if path not in self._arrays:
            self._arrays[path] = np.load(path, mmap_mode="r", allow_pickle=False)
        return self._arrays[path]

    def __getitem__(self, index: int):
        record_index, end = self.index[index]
        record = self.records[record_index]
        features = self._load(record.features_path)
        labels = self._load(record.labels_path)
        sources = self._load(record.sources_path)
        return (
            padded_window(features, end),
            np.array(labels[end - 1], dtype=np.float32, copy=True),
            np.array(sources[end - 1], dtype=np.int64, copy=True),
        )


def _probabilities(model, values, torch):
    onset_logits, source_logits = model(values)
    return torch.sigmoid(onset_logits), torch.softmax(source_logits, dim=-1)


def _balanced_order(dataset: CachedWindowDataset, rng: np.random.Generator,
                    hard_indices: np.ndarray | None = None) -> np.ndarray:
    count = len(dataset)
    have_both_domains = bool(dataset.real_positive_indices or dataset.real_negative_indices) and bool(
        dataset.synthetic_positive_indices or dataset.synthetic_negative_indices
    )
    quotas = (("real", count // 2), ("synthetic", count - count // 2)) if have_both_domains else (
        ("all", count),
    )
    hard = hard_indices.tolist() if hard_indices is not None and len(hard_indices) else []
    sampled: list[np.ndarray] = []
    for domain, quota in quotas:
        if domain == "real":
            positives = dataset.real_positive_indices or dataset.positive_indices
            negatives = dataset.real_negative_indices or dataset.negative_indices
        elif domain == "synthetic":
            positives = dataset.synthetic_positive_indices or dataset.positive_indices
            negatives = dataset.synthetic_negative_indices or dataset.negative_indices
        else:
            positives = dataset.positive_indices
            negatives = dataset.negative_indices
        domain_hard = [index for index in hard if domain == "all" or dataset.domain_by_index[index] == domain]
        negative_pool = domain_hard or negatives
        positive_count = quota // 2
        sampled.append(rng.choice(positives, positive_count, replace=True))
        sampled.append(rng.choice(negative_pool, quota - positive_count, replace=True))
    combined = np.concatenate(sampled).astype(np.int64)
    rng.shuffle(combined)
    return combined


def _match_onsets(truth: list[int], predictions: list[int],
                  tolerance: int = MATCH_TOLERANCE_SAMPLES) -> list[tuple[int, int]]:
    candidates = sorted(
        (abs(prediction - onset), truth_index, prediction_index)
        for truth_index, onset in enumerate(truth)
        for prediction_index, prediction in enumerate(predictions)
        if abs(prediction - onset) <= tolerance
    )
    used_truth: set[int] = set()
    used_predictions: set[int] = set()
    pairs: list[tuple[int, int]] = []
    for _, truth_index, prediction_index in candidates:
        if truth_index in used_truth or prediction_index in used_predictions:
            continue
        used_truth.add(truth_index)
        used_predictions.add(prediction_index)
        pairs.append((truth_index, prediction_index))
    return pairs


def peak_candidates(
    samples: np.ndarray, probabilities: np.ndarray, activities: np.ndarray,
    threshold_quiet: float, threshold_busy: float, scene_cutoff: float,
    minimum_spacing_samples: int, lookahead_frames: int = PEAK_LOOKAHEAD_FRAMES,
) -> list[tuple[int, float, int]]:
    candidates: list[tuple[int, float, int]] = []
    for index in range(len(probabilities)):
        probability = float(probabilities[index])
        threshold = threshold_quiet if float(activities[index]) < scene_cutoff else threshold_busy
        left = max(0, index - lookahead_frames)
        right = min(len(probabilities), index + lookahead_frames + 1)
        local = probabilities[left:right]
        if probability < threshold or probability < float(local.max()):
            continue
        # Select the earliest frame of a flat maximum.  Without this rule a
        # sustained plateau can create periodic false "burst" events even when
        # no probability rearm is required.
        if index > left and probability <= float(probabilities[left:index].max()):
            continue
        if candidates and int(samples[index]) - candidates[-1][0] < minimum_spacing_samples:
            if probability > candidates[-1][1]:
                candidates[-1] = (int(samples[index]), probability, index)
            continue
        candidates.append((int(samples[index]), probability, index))
    return candidates


def collect_development_sequences(model, records: list[CacheRecord], torch,
                                  batch_size: int = 128) -> list[dict]:
    sequences: list[dict] = []
    model.eval()
    with torch.no_grad():
        for record in records:
            features = np.load(record.features_path, mmap_mode="r", allow_pickle=False)
            activities = np.load(record.activity_path, mmap_mode="r", allow_pickle=False)
            ends = list(range(1, len(features) + 1, INFERENCE_STRIDE_FRAMES))
            if ends[-1] != len(features):
                ends.append(len(features))
            onset_batches: list[np.ndarray] = []
            source_batches: list[np.ndarray] = []
            for start in range(0, len(ends), batch_size):
                windows = np.stack([padded_window(features, end) for end in ends[start:start + batch_size]])
                onset, source = _probabilities(model, torch.from_numpy(windows), torch)
                onset_batches.append(onset[:, -1].numpy())
                source_batches.append(source[:, -1].numpy())
            sequences.append({
                "samples": np.asarray([(end - 1) * HOP_SIZE + FFT_SIZE for end in ends], dtype=np.int64),
                "probabilities": np.concatenate(onset_batches),
                "sources": np.concatenate(source_batches),
                "activity": np.asarray(activities[np.asarray(ends) - 1], dtype=np.float32),
                "truth": _usable_events(record.prefix.with_suffix(".jsonl")),
                "prefix": record.prefix,
                "gain_db": record.gain_db,
            })
    return sequences


def _scene_cutoff(sequences: list[dict]) -> float:
    background: list[np.ndarray] = []
    for sequence in sequences:
        mask = np.ones(len(sequence["samples"]), dtype=bool)
        for event in sequence["truth"]:
            onset = int(event["onset_sample"])
            mask &= np.abs(sequence["samples"] - onset) > SAMPLE_RATE // 4
        background.append(sequence["activity"][mask])
    values = np.concatenate(background) if background else np.asarray([0.35], dtype=np.float32)
    return float(np.clip(np.percentile(values, 60), 0.05, 0.95))


def calibrate_policy(sequences: list[dict]) -> dict[str, float | int]:
    cutoff = _scene_cutoff(sequences)
    policy: dict[str, float | int] = {
        "scene_activity_cutoff": cutoff,
        "peak_lookahead_frames": PEAK_LOOKAHEAD_FRAMES,
    }
    threshold_values = np.linspace(0.10, 0.95, 18)
    for class_index, name in enumerate(CLASS_NAMES):
        pairs = [(float(value), float(value)) for value in threshold_values]
        if name == "footstep":
            pairs = [(float(quiet), float(busy)) for quiet in threshold_values
                     for busy in threshold_values if busy >= quiet]
        best_score = (False, float("-inf"), float("-inf"), float("-inf"), float("-inf"))
        best = (0.5, 0.5)
        for quiet_threshold, busy_threshold in pairs:
            tp = fp = fn = ambient_fp = 0
            ambient_minutes = 0.0
            total_minutes = 0.0
            for sequence in sequences:
                candidates = peak_candidates(
                    sequence["samples"], sequence["probabilities"][:, class_index],
                    sequence["activity"], quiet_threshold, busy_threshold, cutoff,
                    MIN_SPACING_MS[name] * SAMPLE_RATE // 1000,
                )
                predictions = [candidate[0] for candidate in candidates]
                truth = [int(event["onset_sample"]) for event in sequence["truth"] if event["class"] == name]
                matched = _match_onsets(truth, predictions)
                tp += len(matched)
                fp += len(predictions) - len(matched)
                fn += len(truth) - len(matched)
                has_targets = any(event["class"] in CLASS_NAMES for event in sequence["truth"])
                if len(sequence["samples"]):
                    total_minutes += float(sequence["samples"][-1]) / SAMPLE_RATE / 60.0
                if not has_targets:
                    ambient_fp += len(predictions)
                    if len(sequence["samples"]):
                        ambient_minutes += float(sequence["samples"][-1]) / SAMPLE_RATE / 60.0
            recall = tp / max(1, tp + fn)
            precision = tp / max(1, tp + fp)
            ambient_rate = ambient_fp / max(ambient_minutes, 1e-12)
            overall_rate = fp / max(total_minutes, 1e-12)
            feasible = ambient_rate <= 0.1 and overall_rate <= 2.0
            violation = max(0.0, ambient_rate - 0.1) + max(0.0, overall_rate - 2.0)
            score = (feasible, -violation, recall, precision, -(ambient_rate + overall_rate))
            if score > best_score:
                best_score = score
                best = (quiet_threshold, busy_threshold)
        policy[f"threshold_quiet_{name}"] = best[0]
        policy[f"threshold_busy_{name}"] = best[1]
        policy[f"minimum_spacing_ms_{name}"] = MIN_SPACING_MS[name]

    # Calibrate acoustic timestamp offsets and the self suppression threshold on
    # event-matched development predictions.
    onset_errors: dict[str, list[int]] = {name: [] for name in CLASS_NAMES}
    self_scores: list[float] = []
    remote_scores: list[float] = []
    for sequence in sequences:
        for class_index, name in enumerate(CLASS_NAMES):
            candidates = peak_candidates(
                sequence["samples"], sequence["probabilities"][:, class_index],
                sequence["activity"], float(policy[f"threshold_quiet_{name}"]),
                float(policy[f"threshold_busy_{name}"]), cutoff,
                MIN_SPACING_MS[name] * SAMPLE_RATE // 1000,
            )
            truth_events = [event for event in sequence["truth"] if event["class"] == name]
            matches = _match_onsets(
                [int(event["onset_sample"]) for event in truth_events],
                [candidate[0] for candidate in candidates],
            )
            for truth_index, prediction_index in matches:
                event = truth_events[truth_index]
                sample, _, row = candidates[prediction_index]
                onset_errors[name].append(sample - int(event["onset_sample"]))
                probability_self = float(sequence["sources"][row, class_index, SOURCE_NAMES.index("self")])
                if event.get("source_hint") == "self":
                    self_scores.append(probability_self)
                elif event.get("source_hint") == "remote":
                    remote_scores.append(probability_self)
    for name in CLASS_NAMES:
        values = onset_errors[name]
        policy[f"onset_offset_samples_{name}"] = int(np.clip(
            np.median(values) if values else FFT_SIZE // 2, 0, MATCH_TOLERANCE_SAMPLES
        ))
    best_source = (float("-inf"), float("-inf"), 0.99)
    for threshold in np.linspace(0.50, 0.99, 50):
        self_rate = float(np.mean(np.asarray(self_scores) >= threshold)) if self_scores else 0.0
        remote_loss = float(np.mean(np.asarray(remote_scores) >= threshold)) if remote_scores else 0.0
        if remote_loss <= 0.03 and (self_rate, -remote_loss, float(threshold)) > best_source:
            best_source = (self_rate, -remote_loss, float(threshold))
    policy["self_suppression_threshold"] = best_source[2]
    return policy


def train_and_export(
    train_prefixes: list[Path],
    dev_prefixes: list[Path],
    output_dir: str | Path,
    epochs: int = 20,
    seed: int = 20260720,
    cache_dir: str | Path | None = None,
    threads: int = 0,
    batch_size: int = 64,
    resume: str | Path | None = None,
) -> Path:
    torch = require_torch()
    if epochs <= 0 or batch_size <= 0:
        raise ValueError("epochs and batch size must be positive")
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.use_deterministic_algorithms(True, warn_only=True)
    if threads > 0:
        torch.set_num_threads(threads)

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    cache_root = Path(cache_dir) if cache_dir else output / "feature-cache"
    capture_gains_db = (-24.0, -12.0, 0.0)
    train_records = prepare_feature_cache(train_prefixes, cache_root / "train", capture_gains_db)
    dev_records = prepare_feature_cache(dev_prefixes, cache_root / "dev", (0.0,))
    dataset = CachedWindowDataset(train_records)
    model = build_model()
    optimizer = torch.optim.AdamW(model.parameters(), lr=8e-4, weight_decay=1e-4)
    start_epoch = 0
    if resume:
        checkpoint = torch.load(resume, map_location="cpu", weights_only=False)
        if checkpoint.get("preprocessing_version") != PREPROCESSING_VERSION:
            raise ValueError("resume checkpoint does not use the current preprocessing contract")
        model.load_state_dict(checkpoint["model"])
        optimizer.load_state_dict(checkpoint["optimizer"])
        start_epoch = int(checkpoint["epoch"])
        if start_epoch > epochs:
            raise ValueError("resume checkpoint is newer than the requested epoch count")

    class_counts = np.zeros(len(CLASS_NAMES), dtype=np.int64)
    for record in train_records:
        labels = np.load(record.labels_path, mmap_mode="r", allow_pickle=False)
        class_counts += (labels >= 0.5).sum(axis=0)
    positive_weights = np.clip(class_counts.max() / np.maximum(class_counts, 1), 1.0, 8.0)
    positive_weight_tensor = torch.from_numpy(positive_weights.astype(np.float32))
    rng = np.random.default_rng(seed)

    def train_epoch(order: np.ndarray) -> float:
        model.train()
        sampler = torch.utils.data.SubsetRandomSampler(order.tolist(), generator=torch.Generator().manual_seed(seed))
        loader = torch.utils.data.DataLoader(
            dataset, batch_size=batch_size, sampler=sampler, num_workers=0,
            pin_memory=False, drop_last=False,
        )
        losses: list[float] = []
        for values, labels, sources in loader:
            values = values.float()
            labels = labels.float()
            sources = sources.long()
            optimizer.zero_grad(set_to_none=True)
            onset_logits, source_logits = model(values)
            onset_logits = onset_logits[:, -1]
            source_logits = source_logits[:, -1]
            base = torch.nn.functional.binary_cross_entropy_with_logits(
                onset_logits, labels, pos_weight=positive_weight_tensor, reduction="none"
            )
            probabilities = torch.sigmoid(onset_logits)
            correctness = labels * probabilities + (1.0 - labels) * (1.0 - probabilities)
            onset_loss = ((1.0 - correctness).pow(2.0) * base).mean()
            source_mask = (labels >= 0.5) & (sources >= 0)
            source_loss = (
                torch.nn.functional.cross_entropy(source_logits[source_mask], sources[source_mask])
                if bool(source_mask.any()) else torch.zeros((), dtype=onset_loss.dtype)
            )
            loss = onset_loss + 0.35 * source_loss
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            optimizer.step()
            losses.append(float(loss.detach()))
        return float(np.mean(losses)) if losses else 0.0

    hard_indices: np.ndarray | None = None
    for epoch in range(start_epoch, epochs):
        loss = train_epoch(_balanced_order(dataset, rng, hard_indices))
        checkpoint_path = output / "training-checkpoint.pt"
        torch.save({
            "epoch": epoch + 1, "model": model.state_dict(), "optimizer": optimizer.state_dict(),
            "seed": seed, "preprocessing_version": PREPROCESSING_VERSION,
        }, checkpoint_path)
        print(f"training epoch {epoch + 1}/{epochs} loss={loss:.6f}", flush=True)

        # Refresh hard negatives after the first pass and at the halfway point.
        if epoch in (0, max(0, epochs // 2 - 1)):
            model.eval()
            mined: list[int] = []
            with torch.no_grad():
                negative_indices = dataset.negative_indices
                for start in range(0, len(negative_indices), batch_size):
                    indices = negative_indices[start:start + batch_size]
                    values = np.stack([dataset[index][0] for index in indices])
                    onset, _ = _probabilities(model, torch.from_numpy(values), torch)
                    hard = onset[:, -1].max(dim=1).values.numpy() >= 0.20
                    mined.extend(index for index, is_hard in zip(indices, hard, strict=True) if is_hard)
            hard_indices = np.asarray(mined, dtype=np.int64)
            print(f"mined {len(hard_indices)} hard-negative windows", flush=True)

    real_dev_records = [record for record in dev_records if record.domain == "real"]
    calibration_records = real_dev_records or dev_records
    development_sequences = collect_development_sequences(model, calibration_records, torch, batch_size)
    policy = calibrate_policy(development_sequences)

    class ExportedModel(torch.nn.Module):
        def __init__(self, inner):
            super().__init__()
            self.inner = inner

        def forward(self, values):
            onset_logits, source_logits = self.inner(values)
            return torch.sigmoid(onset_logits), torch.softmax(source_logits, dim=-1)

    exported_model = ExportedModel(model).eval()
    dummy = torch.zeros((1, INPUT_CHANNELS, CONTEXT_FRAMES, MEL_BINS), dtype=torch.float32)
    dummy[:, 1] = -100.0
    model_path = output / "recognizer.onnx"
    torch.onnx.export(
        exported_model, dummy, model_path, input_names=["features"],
        output_names=["onset_probabilities", "source_probabilities"],
        dynamic_axes=None, opset_version=17, dynamo=False,
    )
    digest = hashlib.sha256(model_path.read_bytes()).hexdigest()
    metadata: dict[str, object] = {
        "package_version": 4,
        "model_version": f"cs2-recognizer-v4-{seed}",
        "model_file": model_path.name,
        "model_sha256": digest,
        "preprocessing_version": PREPROCESSING_VERSION,
        "sample_rate": SAMPLE_RATE,
        "fft_size": FFT_SIZE,
        "hop_size": HOP_SIZE,
        "mel_bins": MEL_BINS,
        "context_frames": CONTEXT_FRAMES,
        "input_channels": INPUT_CHANNELS,
        "inference_stride_frames": INFERENCE_STRIDE_FRAMES,
        "class_order": ",".join(CLASS_NAMES),
        "source_order": ",".join(SOURCE_NAMES),
        "event_mode": "onset-peak",
        "pulse_ms": 50,
        "training_capture_gain_db": ",".join(str(int(value)) for value in capture_gains_db),
        "pcen_smoothing": PCEN_SMOOTHING,
        "pcen_alpha": PCEN_ALPHA,
        "pcen_delta": PCEN_DELTA,
        "pcen_root": PCEN_ROOT,
        "pcen_epsilon": PCEN_EPSILON,
        **policy,
    }
    (output / "model.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    with torch.no_grad():
        torch_onset, torch_source = exported_model(dummy)
    np.savez_compressed(
        output / "onnx_parity.npz", input=dummy.numpy(),
        torch_onset=torch_onset.numpy(), torch_source=torch_source.numpy(),
    )
    (output / "training-summary.json").write_text(json.dumps({
        "train_sessions": len(train_prefixes), "dev_sessions": len(dev_prefixes),
        "real_cached_records": sum(record.domain == "real" for record in train_records),
        "synthetic_cached_records": sum(record.domain == "synthetic" for record in train_records),
        "calibration_domain": "real" if real_dev_records else "synthetic",
        "cached_windows": len(dataset), "positive_windows": len(dataset.positive_indices),
        "negative_windows": len(dataset.negative_indices), "epochs": epochs,
        "batch_size": batch_size, "threads": threads or "torch-default", "policy": policy,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return output
