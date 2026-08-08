from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from pathlib import Path

import numpy as np

from . import CLASS_NAMES, SAMPLE_RATE
from .audio import Audio, load_pcm_wav, to_stereo_48k, write_pcm16_wav
from .inference import PredictedEvent, postprocess, predict_wav, probabilities_for_audio
from .sessions import load_timeline


MATCH_TOLERANCE = SAMPLE_RATE // 10


@dataclass
class Metrics:
    true_positives: int = 0
    false_positives: int = 0
    false_negatives: int = 0
    precision: float = 0.0
    recall: float = 0.0
    f1: float = 0.0
    false_alerts_per_minute: float = 0.0
    median_delivery_latency_ms: float = 0.0
    p95_delivery_latency_ms: float = 0.0
    median_absolute_onset_error_ms: float = 0.0
    p95_absolute_onset_error_ms: float = 0.0
    support: int = 0
    recall_ci95_low: float = 0.0
    recall_ci95_high: float = 0.0
    conclusive: bool = False


def _target_truth(events: list[dict]) -> list[dict]:
    return [
        event for event in events if event.get("class") in CLASS_NAMES
        and not bool(event.get("uncertain", False))
        and bool(event.get("reviewed", True))
    ]


def _derive_scene_modes(events: list[dict], trace) -> list[dict]:
    """Fill real-session scene labels from causal pre-onset activity."""
    cutoff = float(trace.metadata.get("scene_activity_cutoff", 0.5))
    output: list[dict] = []
    for event in events:
        if event.get("scene_mode") in ("quiet", "busy") or not len(trace.samples):
            output.append(event)
            continue
        probe = max(0, int(event.get("onset_sample", 0)) - SAMPLE_RATE // 10)
        row = int(np.searchsorted(trace.samples, probe, side="right") - 1)
        row = max(0, min(row, len(trace.activity) - 1))
        output.append({
            **event,
            "scene_mode": "quiet" if float(trace.activity[row]) < cutoff else "busy",
        })
    return output


def _wilson(successes: int, total: int, z: float = 1.959963984540054) -> tuple[float, float]:
    if total == 0:
        return 0.0, 1.0
    proportion = successes / total
    denominator = 1.0 + z * z / total
    center = (proportion + z * z / (2.0 * total)) / denominator
    margin = z * np.sqrt(
        proportion * (1.0 - proportion) / total + z * z / (4.0 * total * total)
    ) / denominator
    return float(max(0.0, center - margin)), float(min(1.0, center + margin))


def match_events(truth: list[dict], predictions: list[PredictedEvent],
                 tolerance: int = MATCH_TOLERANCE):
    candidates = sorted(
        (abs(prediction.onset_sample - int(event["onset_sample"])), truth_index, prediction_index)
        for truth_index, event in enumerate(truth)
        for prediction_index, prediction in enumerate(predictions)
        if event.get("class") == prediction.sound_class
        and abs(prediction.onset_sample - int(event["onset_sample"])) <= tolerance
    )
    matched_truth: set[int] = set()
    matched_predictions: set[int] = set()
    pairs: list[tuple[int, int]] = []
    for _, truth_index, prediction_index in candidates:
        if truth_index in matched_truth or prediction_index in matched_predictions:
            continue
        matched_truth.add(truth_index)
        matched_predictions.add(prediction_index)
        pairs.append((truth_index, prediction_index))
    return pairs, matched_truth, matched_predictions


def calculate_metrics(truth: list[dict], predictions: list[PredictedEvent],
                      duration_seconds: float) -> dict[str, Metrics]:
    truth = _target_truth(truth)
    predictions = [event for event in predictions if not event.suppressed]
    pairs, matched_truth, matched_predictions = match_events(truth, predictions)
    output = {name: Metrics() for name in CLASS_NAMES}
    onset_errors = {name: [] for name in CLASS_NAMES}
    delivery_latencies = {name: [] for name in CLASS_NAMES}
    for truth_index, prediction_index in pairs:
        name = truth[truth_index]["class"]
        prediction = predictions[prediction_index]
        truth_onset = int(truth[truth_index]["onset_sample"])
        output[name].true_positives += 1
        onset_errors[name].append((prediction.onset_sample - truth_onset) * 1000.0 / SAMPLE_RATE)
        delivery_latencies[name].append((prediction.detected_sample - truth_onset) * 1000.0 / SAMPLE_RATE)
    for index, event in enumerate(truth):
        if index not in matched_truth:
            output[event["class"]].false_negatives += 1
    for index, event in enumerate(predictions):
        if index not in matched_predictions and event.sound_class in output:
            output[event.sound_class].false_positives += 1
    for name, metrics in output.items():
        metrics.precision = metrics.true_positives / max(1, metrics.true_positives + metrics.false_positives)
        metrics.recall = metrics.true_positives / max(1, metrics.true_positives + metrics.false_negatives)
        metrics.f1 = 2 * metrics.precision * metrics.recall / max(1e-12, metrics.precision + metrics.recall)
        metrics.false_alerts_per_minute = metrics.false_positives * 60.0 / max(duration_seconds, 1e-12)
        absolute = np.abs(onset_errors[name])
        delivery = np.asarray(delivery_latencies[name])
        metrics.median_absolute_onset_error_ms = float(np.median(absolute)) if len(absolute) else 0.0
        metrics.p95_absolute_onset_error_ms = float(np.percentile(absolute, 95)) if len(absolute) else 0.0
        metrics.median_delivery_latency_ms = float(np.median(delivery)) if len(delivery) else 0.0
        metrics.p95_delivery_latency_ms = float(np.percentile(delivery, 95)) if len(delivery) else 0.0
        metrics.support = metrics.true_positives + metrics.false_negatives
        metrics.recall_ci95_low, metrics.recall_ci95_high = _wilson(metrics.true_positives, metrics.support)
        metrics.conclusive = metrics.support >= 300
    return output


def recall_subset(truth: list[dict], predictions: list[PredictedEvent], predicate) -> dict[str, dict]:
    truth = _target_truth(truth)
    _, matched_truth, _ = match_events(truth, [event for event in predictions if not event.suppressed])
    output: dict[str, dict] = {}
    for name in CLASS_NAMES:
        indices = [index for index, event in enumerate(truth) if event["class"] == name and predicate(event)]
        hits = sum(index in matched_truth for index in indices)
        low, high = _wilson(hits, len(indices))
        output[name] = {
            "matched": hits, "support": len(indices), "recall": hits / max(1, len(indices)),
            "recall_ci95_low": low, "recall_ci95_high": high, "conclusive": len(indices) >= 300,
        }
    return output


def _rapid_gunshots(truth: list[dict], predictions: list[PredictedEvent]) -> dict:
    targets = _target_truth(truth)
    _, matched, _ = match_events(targets, [event for event in predictions if not event.suppressed])
    indices = sorted(
        (index for index, event in enumerate(targets) if event["class"] == "gunshot"),
        key=lambda index: int(targets[index]["onset_sample"]),
    )
    minimum = 60 * SAMPLE_RATE // 1000
    maximum = 250 * SAMPLE_RATE // 1000
    pairs = [(first, second) for first, second in zip(indices, indices[1:])
             if minimum <= int(targets[second]["onset_sample"]) - int(targets[first]["onset_sample"]) <= maximum]
    passed = sum(first in matched and second in matched for first, second in pairs)
    return {"passed": passed, "support": len(pairs), "rate": passed / max(1, len(pairs))}


def _duplicate_rate(truth: list[dict], predictions: list[PredictedEvent]) -> float:
    targets = _target_truth(truth)
    emitted = [event for event in predictions if not event.suppressed]
    pairs, _, matched_predictions = match_events(targets, emitted)
    matched_truth_onsets = [
        (targets[truth_index]["class"], int(targets[truth_index]["onset_sample"]))
        for truth_index, _ in pairs
    ]
    duplicates = sum(
        prediction_index not in matched_predictions and any(
            prediction.sound_class == name
            and abs(prediction.onset_sample - onset) <= MATCH_TOLERANCE
            for name, onset in matched_truth_onsets
        )
        for prediction_index, prediction in enumerate(emitted)
    )
    return duplicates / max(1, len(pairs))


def _source_metrics(truth: list[dict], raw_predictions: list[PredictedEvent],
                    emitted_predictions: list[PredictedEvent]) -> dict:
    targets = _target_truth(truth)
    raw_pairs, raw_matched, _ = match_events(targets, raw_predictions)
    emitted_pairs, emitted_matched, _ = match_events(targets, emitted_predictions)
    del raw_pairs, emitted_pairs
    self_indices = [index for index, event in enumerate(targets) if event.get("source_hint") == "self"]
    remote_indices = [index for index, event in enumerate(targets) if event.get("source_hint") == "remote"]
    raw_self = sum(index in raw_matched for index in self_indices)
    emitted_self = sum(index in emitted_matched for index in self_indices)
    raw_remote = sum(index in raw_matched for index in remote_indices)
    emitted_remote = sum(index in emitted_matched for index in remote_indices)
    return {
        "self_support": len(self_indices),
        "raw_self_recall": raw_self / max(1, len(self_indices)),
        "self_suppression_rate": (raw_self - emitted_self) / max(1, raw_self),
        "remote_support": len(remote_indices),
        "raw_remote_recall": raw_remote / max(1, len(remote_indices)),
        "emitted_remote_recall": emitted_remote / max(1, len(remote_indices)),
        "remote_recall_loss": (raw_remote - emitted_remote) / max(1, len(remote_indices)),
    }


def _error_clips(output: Path, audio: Audio, truth: list[dict], predictions: list[PredictedEvent]) -> None:
    targets = _target_truth(truth)
    emitted = [event for event in predictions if not event.suppressed]
    _, matched_truth, matched_predictions = match_events(targets, emitted)
    clips = output / "errors"
    clips.mkdir(parents=True, exist_ok=True)
    stereo = to_stereo_48k(audio)
    context = SAMPLE_RATE // 2
    errors: list[tuple[str, str, int, int]] = []
    for index, event in enumerate(targets):
        if index not in matched_truth:
            errors.append(("miss", event["class"], int(event["onset_sample"]), index))
    for index, event in enumerate(emitted):
        if index not in matched_predictions:
            errors.append(("false_positive", event.sound_class, event.onset_sample, index))
    for kind, name, center, index in errors:
        start = max(0, center - context)
        end = min(len(stereo.samples), center + context)
        write_pcm16_wav(clips / f"{kind}_{name}_{index:05d}.wav",
                        Audio(stereo.samples[start:end], SAMPLE_RATE))


def evaluate_sessions(sessions_dir: str | Path, package_dir: str | Path,
                      output_dir: str | Path) -> dict:
    sessions = Path(sessions_dir)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    aggregate_truth: list[dict] = []
    aggregate_emitted: list[PredictedEvent] = []
    aggregate_raw: list[PredictedEvent] = []
    target_free_predictions = {name: 0 for name in CLASS_NAMES}
    target_free_duration = 0.0
    complex_predictions = {name: 0 for name in CLASS_NAMES}
    complex_duration = 0.0
    duration_total = 0.0
    offset = 0
    per_session: dict[str, dict] = {}

    for wav_path in sorted(sessions.glob("test_*.wav")):
        prefix = wav_path.with_suffix("")
        timeline = load_timeline(prefix.with_suffix(".jsonl"))
        raw_events, trace = predict_wav(wav_path, package_dir, include_suppressed=True)
        timeline = _derive_scene_modes(timeline, trace)
        emitted = [event for event in raw_events if not event.suppressed]
        audio = to_stereo_48k(load_pcm_wav(wav_path))
        duration = len(audio.samples) / SAMPLE_RATE
        metrics = calculate_metrics(timeline, emitted, duration)
        per_session[wav_path.stem] = {
            "metrics": {name: asdict(value) for name, value in metrics.items()},
            "source": _source_metrics(timeline, raw_events, emitted),
        }
        np.savez_compressed(
            output / f"{wav_path.stem}.scores.npz", samples=trace.samples,
            probabilities=trace.probabilities, source_probabilities=trace.source_probabilities,
            activity=trace.activity,
        )
        with (output / f"{wav_path.stem}.predictions.jsonl").open("w", encoding="utf-8") as stream:
            for event in raw_events:
                stream.write(json.dumps(asdict(event), sort_keys=True) + "\n")
        _error_clips(output / wav_path.stem, audio, timeline, emitted)

        targets = _target_truth(timeline)
        if not targets:
            target_free_duration += duration
            for name in CLASS_NAMES:
                target_free_predictions[name] += sum(event.sound_class == name for event in emitted)
        is_complex = "_complex_" in wav_path.name or any(
            event.get("scene_mode") == "busy" for event in timeline
        )
        if is_complex:
            complex_duration += duration
            _, _, matched_prediction_indices = match_events(_target_truth(timeline), emitted)
            for name in CLASS_NAMES:
                complex_predictions[name] += sum(
                    index not in matched_prediction_indices and event.sound_class == name
                    for index, event in enumerate(emitted)
                )

        aggregate_truth.extend({
            **event,
            "onset_sample": int(event["onset_sample"]) + offset,
            "end_sample": int(event["end_sample"]) + offset,
        } for event in timeline)
        aggregate_emitted.extend(PredictedEvent(
            event.sound_class, event.onset_sample + offset, event.end_sample + offset,
            event.confidence, event.detected_sample + offset, event.source_hint,
            event.source_confidence, event.scene_mode, event.suppressed,
        ) for event in emitted)
        aggregate_raw.extend(PredictedEvent(
            event.sound_class, event.onset_sample + offset, event.end_sample + offset,
            event.confidence, event.detected_sample + offset, event.source_hint,
            event.source_confidence, event.scene_mode, event.suppressed,
        ) for event in raw_events)
        offset += len(audio.samples)
        duration_total += duration

    if not per_session:
        raise ValueError("evaluation requires test_*.wav sessions")

    overall = calculate_metrics(aggregate_truth, aggregate_emitted, duration_total)
    quiet_remote = recall_subset(
        aggregate_truth, aggregate_emitted,
        lambda event: event.get("source_hint") == "remote"
        and (event.get("scene_mode") == "quiet" or event.get("stratum") == "simple"),
    )
    busy_remote = recall_subset(
        aggregate_truth, aggregate_emitted,
        lambda event: event.get("source_hint") == "remote"
        and (event.get("scene_mode") == "busy" or event.get("stratum") == "complex"),
    )
    remote_all = recall_subset(
        aggregate_truth, aggregate_emitted,
        lambda event: event.get("source_hint") == "remote",
    )
    source = _source_metrics(aggregate_truth, aggregate_raw, aggregate_emitted)
    rapid = _rapid_gunshots(aggregate_truth, aggregate_emitted)
    ambient_rates = {
        name: target_free_predictions[name] * 60.0 / max(target_free_duration, 1e-12)
        for name in CLASS_NAMES
    }
    complex_rates = {
        name: complex_predictions[name] * 60.0 / max(complex_duration, 1e-12)
        for name in CLASS_NAMES
    }
    duplicate_rate = _duplicate_rate(aggregate_truth, aggregate_emitted)
    target_truth = _target_truth(aggregate_truth)
    remote_support = {
        name: sum(event["class"] == name and event.get("source_hint") == "remote" for event in target_truth)
        for name in CLASS_NAMES
    }
    self_support = {
        name: sum(event["class"] == name and event.get("source_hint") == "self" for event in target_truth)
        for name in CLASS_NAMES
    }
    acceptance = {
        "locked_remote_support": all(value >= 300 for value in remote_support.values()),
        "locked_self_support": all(value >= 200 for value in self_support.values()),
        "quiet_remote_footstep_recall": quiet_remote["footstep"]["support"] >= 100
                                         and quiet_remote["footstep"]["recall"] >= 0.95,
        "remote_gunshot_recall": remote_all["gunshot"]["support"] > 0
                                 and remote_all["gunshot"]["recall"] >= 0.95,
        "busy_remote_gunshot_recall": busy_remote["gunshot"]["support"] >= 100
                                      and busy_remote["gunshot"]["recall"] >= 0.95,
        "rapid_gunshots": rapid["support"] >= 100 and rapid["rate"] >= 0.95,
        "duplicates": duplicate_rate < 0.01,
        "ambient_false_alerts": target_free_duration >= 3600.0
                                and all(rate <= 0.1 for rate in ambient_rates.values()),
        "complex_false_alerts": sum(complex_rates.values()) <= 2.0,
        "onset_accuracy": all(
            value.support > 0 and value.p95_absolute_onset_error_ms <= 30.0
            for value in overall.values()
        ),
        "delivery_latency": all(
            value.support > 0 and value.median_delivery_latency_ms <= 50.0
            and value.p95_delivery_latency_ms <= 100.0 for value in overall.values()
        ),
        "self_suppression": source["self_suppression_rate"] >= 0.98,
        "remote_suppression_loss": source["remote_recall_loss"] <= 0.03,
    }
    report = {
        "report_version": "sound-eval-v4",
        "overall": {name: asdict(value) for name, value in overall.items()},
        "quiet_remote": quiet_remote,
        "busy_remote": busy_remote,
        "remote_all": remote_all,
        "source": source,
        "remote_support": remote_support,
        "self_support": self_support,
        "rapid_gunshots": rapid,
        "duplicate_rate": duplicate_rate,
        "target_free_duration_seconds": target_free_duration,
        "ambient_false_alerts_per_minute": ambient_rates,
        "complex_duration_seconds": complex_duration,
        "complex_false_alerts_per_minute": complex_rates,
        "duration_seconds": duration_total,
        "acceptance": acceptance,
        "acceptance_passed": all(acceptance.values()),
        "sessions": per_session,
    }
    (output / "evaluation.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return report
