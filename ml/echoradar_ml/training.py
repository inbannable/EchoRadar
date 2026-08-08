from __future__ import annotations

import hashlib
import json
from pathlib import Path
import random

import numpy as np

from . import (
    CLASS_NAMES, CONTEXT_FRAMES, FFT_SIZE, HOP_SIZE, INPUT_CHANNELS, MEL_BINS,
    ONSET_PREPROCESSING_VERSION, PCEN_ALPHA, PCEN_DELTA, PCEN_EPSILON, PCEN_ROOT,
    PCEN_SMOOTHING, SAMPLE_RATE,
)
from .audio import load_pcm_wav, to_stereo_48k
from .features import stereo_mel_energy, stereo_pcen_from_energy
from .model import build_model, require_torch


def _load_timeline(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def _targets(frame_count: int, events: list[dict]) -> np.ndarray:
    output = np.zeros((frame_count, len(CLASS_NAMES)), dtype=np.float32)
    for event in events:
        if event["class"] not in CLASS_NAMES:
            continue
        class_index = CLASS_NAMES.index(event["class"])
        # Feature frame i becomes causally available at i * HOP_SIZE + FFT_SIZE.
        # Do not supervise the frame whose Hann window merely ends at the
        # onset: its final coefficient is zero, so it contains no usable onset
        # evidence.  Wait one hop, which places the onset near the centre of
        # the first supervised window while remaining causal (~10.7 ms).
        onset = int(event["onset_sample"])
        first_evidence_sample = onset + HOP_SIZE
        start = max(0, (first_evidence_sample - FFT_SIZE + HOP_SIZE - 1) // HOP_SIZE)
        for offset, weight in enumerate((1.0, 0.5, 0.25)):
            if start + offset < frame_count:
                output[start + offset, class_index] = max(output[start + offset, class_index], weight)
    return output


def _window_ends(labels: np.ndarray, stride_frames: int) -> list[int]:
    ends = set(range(CONTEXT_FRAMES, len(labels) + 1, stride_frames))
    for frame in np.flatnonzero(labels.max(axis=1) > 0.0):
        if frame + 1 >= CONTEXT_FRAMES:
            ends.add(int(frame) + 1)
    return sorted(ends)


def load_windows(session_prefixes: list[Path], stride_frames: int = 24,
                 gain_db_values: tuple[float, ...] = (0.0,)) -> tuple[np.ndarray, np.ndarray]:
    inputs: list[np.ndarray] = []
    targets: list[np.ndarray] = []
    for session_index, prefix in enumerate(session_prefixes):
        audio = to_stereo_48k(load_pcm_wav(prefix.with_suffix(".wav")))
        events = _load_timeline(prefix.with_suffix(".jsonl"))
        energy = stereo_mel_energy(audio.samples)
        for gain_db in gain_db_values:
            features = stereo_pcen_from_energy(energy, float(gain_db))
            labels = _targets(len(features), events)
            for end in _window_ends(labels, stride_frames):
                inputs.append(features[end - CONTEXT_FRAMES : end])
                targets.append(labels[end - 1])
        print(f"features {session_index + 1}/{len(session_prefixes)}: {prefix.name}", flush=True)
    if not inputs:
        raise ValueError("no training windows were generated")
    # Windows are collected as [N, T, C, F]; ONNX consumes [N, C, T, F].
    return np.stack(inputs).transpose(0, 2, 1, 3), np.stack(targets)


def _probabilities(model, values, torch):
    objectness_logits, class_logits = model(values)
    return torch.sigmoid(objectness_logits) * torch.sigmoid(class_logits)


def tune_thresholds(truth: np.ndarray, probabilities: np.ndarray) -> dict[str, float]:
    thresholds: dict[str, float] = {}
    for index, name in enumerate(CLASS_NAMES):
        best = (float("-inf"), 0.5)
        for threshold in np.linspace(0.05, 0.95, 19):
            predicted = probabilities[..., index] >= threshold
            actual = truth[..., index] >= 0.5
            tp = np.logical_and(predicted, actual).sum()
            fp = np.logical_and(predicted, ~actual).sum()
            fn = np.logical_and(~predicted, actual).sum()
            precision = tp / max(1, tp + fp)
            recall = tp / max(1, tp + fn)
            f2 = 5 * precision * recall / max(1e-12, 4 * precision + recall)
            if (f2, -threshold) > best:
                best = (float(f2), float(threshold))
        thresholds[name] = best[1]
    return thresholds


def _event_predictions(samples: np.ndarray, probabilities: np.ndarray, threshold: float,
                       refractory_samples: int) -> list[int]:
    rearm_threshold = threshold * 0.50
    armed = True
    refractory_until = 0
    predictions: list[int] = []
    for sample, probability in zip(samples, probabilities, strict=True):
        sample = int(sample)
        if not armed:
            if probability < rearm_threshold:
                armed = True
            continue
        if sample >= refractory_until and probability >= threshold:
            predictions.append(sample)
            armed = False
            refractory_until = sample + refractory_samples
    return predictions


def _match_onsets(truth_onsets: list[int], predictions: list[int],
                  tolerance: int = 7200) -> tuple[int, int, int, list[int]]:
    matched: set[int] = set()
    true_positives = 0
    errors: list[int] = []
    for prediction in predictions:
        candidates = [(abs(prediction - truth), index) for index, truth in enumerate(truth_onsets)
                      if index not in matched and abs(prediction - truth) <= tolerance]
        if candidates:
            _, index = min(candidates)
            matched.add(index)
            true_positives += 1
            errors.append(abs(prediction - truth_onsets[index]))
    return (true_positives, len(predictions) - true_positives,
            len(truth_onsets) - true_positives, errors)


def _event_counts(truth_onsets: list[int], samples: np.ndarray, probabilities: np.ndarray,
                  threshold: float, refractory_samples: int,
                  tolerance: int = 7200) -> tuple[int, int, int]:
    predictions = _event_predictions(samples, probabilities, threshold, refractory_samples)
    tp, fp, fn, _ = _match_onsets(truth_onsets, predictions, tolerance)
    return tp, fp, fn


def collect_development_sequences(model, session_prefixes: list[Path], torch,
                                  gain_db_values: tuple[float, ...] = (0.0,),
                                  batch_size: int = 256):
    sequences = []
    with torch.no_grad():
        for prefix in session_prefixes:
            audio = to_stereo_48k(load_pcm_wav(prefix.with_suffix(".wav")))
            truth = _load_timeline(prefix.with_suffix(".jsonl"))
            energy = stereo_mel_energy(audio.samples)
            for gain_db in gain_db_values:
                features = stereo_pcen_from_energy(energy, float(gain_db))
                ends = list(range(CONTEXT_FRAMES, len(features) + 1, 2))
                windows = np.stack([features[end - CONTEXT_FRAMES : end] for end in ends])
                windows = windows.transpose(0, 2, 1, 3)
                probabilities = np.concatenate([
                    _probabilities(model, torch.from_numpy(windows[start:start + batch_size]), torch)
                    .numpy()[:, -1, :]
                    for start in range(0, len(windows), batch_size)
                ])
                samples = np.asarray([(end - 1) * HOP_SIZE + FFT_SIZE for end in ends], dtype=np.int64)
                sequences.append((samples, probabilities, truth))
    return sequences


def tune_event_thresholds(sequences) -> dict[str, float]:
    thresholds: dict[str, float] = {}
    for class_index, name in enumerate(CLASS_NAMES):
        best_score = (False, float("-inf"), float("-inf"), float("-inf"),
                      float("-inf"), float("-inf"))
        best_threshold = 0.5
        for threshold in np.linspace(0.10, 0.95, 18):
            tp = fp = fn = 0
            ambient_false_alerts = 0
            ambient_minutes = 0.0
            wrong_class_cotriggers = 0
            confusion_support = 0
            absolute_errors: list[int] = []
            for samples, probabilities, truth in sequences:
                truth_onsets = [int(event["onset_sample"]) for event in truth if event["class"] == name]
                refractory_ms = (40, 60, 80)[class_index]
                predictions = _event_predictions(
                    samples, probabilities[:, class_index], float(threshold),
                    refractory_ms * SAMPLE_RATE // 1000)
                counts = _match_onsets(truth_onsets, predictions)
                tp += counts[0]
                fp += counts[1]
                fn += counts[2]
                absolute_errors.extend(counts[3])
                duration_minutes = len(samples) * 2 * HOP_SIZE / SAMPLE_RATE / 60.0
                if not truth:
                    ambient_false_alerts += len(predictions)
                    ambient_minutes += duration_minutes
                for event in truth:
                    if (event["class"] in ("gunshot", "footstep") and event["class"] != name
                            and not bool(event.get("overlap", False))):
                        confusion_support += 1
                        onset = int(event["onset_sample"])
                        if any(abs(prediction - onset) <= 7200 for prediction in predictions):
                            wrong_class_cotriggers += 1
            recall = tp / max(1, tp + fn)
            ambient_rate = ambient_false_alerts / max(ambient_minutes, 1e-12)
            wrong_rate = wrong_class_cotriggers / max(1, confusion_support)
            feasible = ambient_rate <= 0.1 and wrong_rate < 0.01
            violation = max(0.0, ambient_rate - 0.1) + max(0.0, wrong_rate - 0.01)
            median_error = float(np.median(absolute_errors)) if absolute_errors else float("inf")
            # Lexicographic contract: satisfy alert/co-trigger gates, then recall,
            # then duplicates, acoustic onset error, and finally prefer restraint.
            candidate = (feasible, -violation, recall, -float(fp), -median_error,
                         -float(threshold))
            if candidate > best_score:
                best_score = candidate
                best_threshold = float(threshold)
        thresholds[name] = best_threshold
    return thresholds


def calibrate_onset_offsets(sequences, thresholds: dict[str, float]) -> dict[str, int]:
    errors: dict[str, list[int]] = {name: [] for name in CLASS_NAMES}
    for samples, probabilities, truth in sequences:
        for class_index, name in enumerate(CLASS_NAMES):
            predictions = _event_predictions(
                samples, probabilities[:, class_index], thresholds[name],
                (40, 60, 80)[class_index] * SAMPLE_RATE // 1000,
            )
            truths = [int(event["onset_sample"]) for event in truth if event["class"] == name]
            matched: set[int] = set()
            for prediction in predictions:
                candidates = [(abs(prediction - value), index) for index, value in enumerate(truths)
                              if index not in matched and abs(prediction - value) <= 7200]
                if candidates:
                    _, index = min(candidates)
                    matched.add(index)
                    errors[name].append(prediction - truths[index])
    return {
        name: int(np.clip(np.median(values) if values else FFT_SIZE // 2, 0, 7200))
        for name, values in errors.items()
    }


def train_and_export(
    train_prefixes: list[Path],
    dev_prefixes: list[Path],
    output_dir: str | Path,
    epochs: int = 20,
    seed: int = 20260720,
) -> Path:
    torch = require_torch()
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.use_deterministic_algorithms(True, warn_only=True)

    capture_gains_db = (-24.0, -12.0, 0.0)
    train_x, train_y = load_windows(train_prefixes, gain_db_values=capture_gains_db)
    dev_x, dev_y = load_windows(dev_prefixes, gain_db_values=capture_gains_db)
    model = build_model(INPUT_CHANNELS)
    optimizer = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=1e-4)
    batch_size = 128

    positive_windows = np.flatnonzero(train_y.max(axis=1) > 0.0)
    negative_windows = np.flatnonzero(train_y.max(axis=1) == 0.0)
    if not len(positive_windows) or not len(negative_windows):
        raise ValueError("training requires both onset-bearing and background windows")
    class_positive_counts = (train_y[positive_windows] > 0.0).sum(axis=0)
    positive_weights = np.clip(
        len(positive_windows) / np.maximum(class_positive_counts, 1), 1.0, 5.0
    )
    positive_weight_tensor = torch.from_numpy(positive_weights.astype(np.float32))

    def balanced_order(hard_indices: np.ndarray | None = None):
        half = max(1, len(train_x) // 2)
        positives = positive_windows[torch.randint(len(positive_windows), (half,)).numpy()]
        negative_pool = hard_indices if hard_indices is not None and len(hard_indices) else negative_windows
        negatives = negative_pool[torch.randint(len(negative_pool), (len(train_x) - half,)).numpy()]
        combined = torch.from_numpy(np.concatenate((positives, negatives)))
        return combined[torch.randperm(len(combined))]

    def train_epoch(order):
        model.train()
        for start in range(0, len(order), batch_size):
            indices = order[start:start + batch_size].numpy()
            values = torch.from_numpy(train_x[indices])
            labels = torch.from_numpy(train_y[indices])
            target_objectness = labels.max(dim=1, keepdim=True).values
            optimizer.zero_grad(set_to_none=True)
            objectness_logits, class_logits = model(values)
            objectness_logits = objectness_logits[:, -1]
            class_logits = class_logits[:, -1]
            object_loss = torch.nn.functional.binary_cross_entropy_with_logits(
                objectness_logits, target_objectness, reduction="none"
            )
            class_loss = torch.nn.functional.binary_cross_entropy_with_logits(
                class_logits, labels, pos_weight=positive_weight_tensor, reduction="none"
            )
            object_probability = torch.sigmoid(objectness_logits)
            class_probability = torch.sigmoid(class_logits)
            object_correct = target_objectness * object_probability + (1.0 - target_objectness) * (1.0 - object_probability)
            class_correct = labels * class_probability + (1.0 - labels) * (1.0 - class_probability)
            focal = ((1.0 - object_correct).pow(2.0) * object_loss).mean()
            focal = focal + ((1.0 - class_correct).pow(2.0) * class_loss).mean()

            hard_labels = labels >= 0.5
            single = hard_labels.sum(dim=1) == 1
            target_scores = (class_probability * hard_labels).sum(dim=1)
            non_target_scores = class_probability.masked_fill(hard_labels, 0.0).max(dim=1).values
            margin_values = torch.relu(non_target_scores - target_scores + 0.25)
            margin_loss = margin_values[single].mean() if single.any() else torch.tensor(0.0)
            loss = focal + margin_loss
            loss.backward()
            optimizer.step()

    model.train()
    for epoch in range(epochs):
        train_epoch(balanced_order())
        print(f"training epoch {epoch + 1}/{epochs}", flush=True)

    # Mine false-positive and cross-class-confusion windows, then retrain once.
    model.eval()
    hard_windows: list[np.ndarray] = []
    with torch.no_grad():
        for start in range(0, len(train_x), batch_size):
            values = torch.from_numpy(train_x[start:start + batch_size])
            probabilities = _probabilities(model, values, torch).numpy()[:, -1]
            labels = train_y[start:start + batch_size]
            negative = labels.max(axis=1) == 0.0
            false_positive = probabilities.max(axis=1) >= 0.25
            target_frames = labels.max(axis=1, keepdims=True) >= 0.5
            cross_class = np.logical_and(
                probabilities >= 0.25, np.logical_and(labels < 0.5, target_frames)
            ).any(axis=1)
            hard_windows.append(np.flatnonzero((negative & false_positive) | cross_class) + start)
    hard_indices = np.concatenate(hard_windows) if hard_windows else np.empty(0, dtype=np.int64)
    for epoch in range(max(1, epochs // 5)):
        train_epoch(balanced_order(hard_indices))
        print(f"hard-negative epoch {epoch + 1}/{max(1, epochs // 5)}", flush=True)

    model.eval()
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    torch.save(model.state_dict(), output / "training-checkpoint.pt")
    with torch.no_grad():
        dev_predictions = np.concatenate([
            _probabilities(model, torch.from_numpy(dev_x[start:start + batch_size]), torch)
            .numpy()[:, -1]
            for start in range(0, len(dev_x), batch_size)
        ])
    development_sequences = collect_development_sequences(
        model, dev_prefixes, torch, capture_gains_db, batch_size
    )
    thresholds = tune_event_thresholds(development_sequences)
    onset_offsets = calibrate_onset_offsets(development_sequences, thresholds)

    model_path = output / "recognizer.onnx"
    dummy = torch.zeros((1, INPUT_CHANNELS, CONTEXT_FRAMES, MEL_BINS), dtype=torch.float32)
    class ProbabilityWrapper(torch.nn.Module):
        def __init__(self, inner):
            super().__init__()
            self.inner = inner

        def forward(self, values):
            objectness_logits, class_logits = self.inner(values)
            return torch.sigmoid(objectness_logits) * torch.sigmoid(class_logits)

    exported_model = ProbabilityWrapper(model).eval()
    torch.onnx.export(
        exported_model, dummy, model_path, input_names=["logmel"], output_names=["probabilities"],
        dynamic_axes=None, opset_version=17, dynamo=False,
    )
    digest = hashlib.sha256(model_path.read_bytes()).hexdigest()
    metadata = {
        "model_version": f"cs2-recognizer-onset-{seed}", "model_file": model_path.name,
        "model_sha256": digest, "preprocessing_version": ONSET_PREPROCESSING_VERSION,
        "sample_rate": SAMPLE_RATE, "fft_size": FFT_SIZE, "hop_size": HOP_SIZE,
        "mel_bins": MEL_BINS, "context_frames": CONTEXT_FRAMES, "input_channels": INPUT_CHANNELS,
        "inference_stride_frames": 2, "class_order": ",".join(CLASS_NAMES),
        "training_capture_gain_db": ",".join(str(int(value)) for value in capture_gains_db),
        "event_mode": "onset-pulse", "pulse_ms": 50,
        "pcen_smoothing": PCEN_SMOOTHING, "pcen_alpha": PCEN_ALPHA,
        "pcen_delta": PCEN_DELTA, "pcen_root": PCEN_ROOT, "pcen_epsilon": PCEN_EPSILON,
    }
    for class_index, name in enumerate(CLASS_NAMES):
        metadata[f"threshold_{name}"] = thresholds[name]
        metadata[f"off_threshold_{name}"] = round(thresholds[name] * 0.50, 6)
        metadata[f"rearm_threshold_{name}"] = round(thresholds[name] * 0.50, 6)
        metadata[f"min_on_frames_{name}"] = 1
        metadata[f"min_off_frames_{name}"] = 1
        metadata[f"refractory_ms_{name}"] = (40, 60, 80)[class_index]
        metadata[f"onset_offset_samples_{name}"] = onset_offsets[name]
    (output / "model.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    np.savez_compressed(output / "dev_predictions.npz", truth=dev_y, probabilities=dev_predictions)
    np.savez_compressed(output / "onnx_parity.npz", input=train_x[:1],
                        torch_output=exported_model(torch.from_numpy(train_x[:1])).detach().numpy())
    return output
