from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from pathlib import Path

import numpy as np

from . import CLASS_NAMES, SAMPLE_RATE
from .audio import Audio, load_pcm_wav, to_stereo_48k, write_pcm16_wav
from .inference import PredictedEvent, postprocess, predict_wav, probabilities_for_audio


@dataclass
class Metrics:
    true_positives: int = 0
    false_positives: int = 0
    false_negatives: int = 0
    precision: float = 0.0
    recall: float = 0.0
    f1: float = 0.0
    false_alerts_per_minute: float = 0.0
    mean_latency_ms: float = 0.0
    mean_detection_latency_ms: float = 0.0
    median_absolute_onset_error_ms: float = 0.0
    p95_absolute_onset_error_ms: float = 0.0
    support: int = 0
    recall_ci95_low: float = 0.0
    recall_ci95_high: float = 0.0
    conclusive: bool = False


def _wilson(successes: int, total: int, z: float = 1.959963984540054) -> tuple[float, float]:
    if total == 0:
        return 0.0, 1.0
    proportion = successes / total
    denominator = 1.0 + z * z / total
    center = (proportion + z * z / (2.0 * total)) / denominator
    margin = z * np.sqrt(proportion * (1.0 - proportion) / total + z * z / (4.0 * total * total)) / denominator
    return float(max(0.0, center - margin)), float(min(1.0, center + margin))


def load_timeline(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def match_events(truth: list[dict], predictions: list[PredictedEvent], tolerance: int = 7200):
    matched_truth: set[int] = set()
    matched_predictions: set[int] = set()
    pairs: list[tuple[int, int]] = []
    for prediction_index, prediction in enumerate(predictions):
        candidates = [
            (abs(prediction.onset_sample - int(event["onset_sample"])), truth_index)
            for truth_index, event in enumerate(truth)
            if truth_index not in matched_truth and event["class"] == prediction.sound_class
            and abs(prediction.onset_sample - int(event["onset_sample"])) <= tolerance
        ]
        if not candidates:
            continue
        _, truth_index = min(candidates)
        matched_truth.add(truth_index)
        matched_predictions.add(prediction_index)
        pairs.append((truth_index, prediction_index))
    return pairs, matched_truth, matched_predictions


def calculate_metrics(truth: list[dict], predictions: list[PredictedEvent], duration_seconds: float) -> dict[str, Metrics]:
    pairs, matched_truth, matched_predictions = match_events(truth, predictions)
    output = {name: Metrics() for name in CLASS_NAMES}
    latencies = {name: [] for name in CLASS_NAMES}
    detection_latencies = {name: [] for name in CLASS_NAMES}
    for truth_index, prediction_index in pairs:
        name = truth[truth_index]["class"]
        output[name].true_positives += 1
        latencies[name].append((predictions[prediction_index].onset_sample - int(truth[truth_index]["onset_sample"]))
                               * 1000.0 / SAMPLE_RATE)
        detection_sample = predictions[prediction_index].detected_sample or predictions[prediction_index].onset_sample
        detection_latencies[name].append(
            (detection_sample - int(truth[truth_index]["onset_sample"])) * 1000.0 / SAMPLE_RATE
        )
    for index, event in enumerate(truth):
        if index not in matched_truth and event["class"] in output:
            output[event["class"]].false_negatives += 1
    for index, event in enumerate(predictions):
        if index not in matched_predictions:
            output[event.sound_class].false_positives += 1
    for name, metrics in output.items():
        metrics.precision = metrics.true_positives / max(1, metrics.true_positives + metrics.false_positives)
        metrics.recall = metrics.true_positives / max(1, metrics.true_positives + metrics.false_negatives)
        metrics.f1 = 2 * metrics.precision * metrics.recall / max(1e-12, metrics.precision + metrics.recall)
        metrics.false_alerts_per_minute = metrics.false_positives * 60.0 / max(1e-12, duration_seconds)
        metrics.mean_latency_ms = float(np.mean(latencies[name])) if latencies[name] else 0.0
        metrics.mean_detection_latency_ms = (
            float(np.mean(detection_latencies[name])) if detection_latencies[name] else 0.0
        )
        absolute_errors = np.abs(latencies[name])
        metrics.median_absolute_onset_error_ms = (
            float(np.median(absolute_errors)) if len(absolute_errors) else 0.0
        )
        metrics.p95_absolute_onset_error_ms = (
            float(np.percentile(absolute_errors, 95)) if len(absolute_errors) else 0.0
        )
        metrics.support = metrics.true_positives + metrics.false_negatives
        metrics.recall_ci95_low, metrics.recall_ci95_high = _wilson(metrics.true_positives, metrics.support)
        metrics.conclusive = metrics.support >= 30
    return output


def recall_breakdowns(truth: list[dict], predictions: list[PredictedEvent]) -> dict[str, dict[str, dict]]:
    _, matched_truth, _ = match_events(truth, predictions)
    buckets = {
        "overlap": lambda event: bool(event.get("overlap", False)),
        "non_overlap": lambda event: not bool(event.get("overlap", False)),
        "snr_below_0db": lambda event: float(event.get("snr_db", 0.0)) < 0.0,
        "snr_0_to_10db": lambda event: 0.0 <= float(event.get("snr_db", 0.0)) < 10.0,
        "snr_at_least_10db": lambda event: float(event.get("snr_db", 0.0)) >= 10.0,
        "seen_source": lambda event: bool(event.get("seen_source", False)),
        "unseen_source": lambda event: not bool(event.get("seen_source", False)),
        "self_rendered": lambda event: event.get("render_mode", "self") == "self",
        "remote_rendered": lambda event: event.get("render_mode") == "remote",
        "remote_front": lambda event: event.get("render_mode") == "remote" and
                                      event.get("azimuth_quadrant") == "front",
        "remote_right": lambda event: event.get("render_mode") == "remote" and
                                      event.get("azimuth_quadrant") == "right",
        "remote_rear": lambda event: event.get("render_mode") == "remote" and
                                     event.get("azimuth_quadrant") == "rear",
        "remote_left": lambda event: event.get("render_mode") == "remote" and
                                     event.get("azimuth_quadrant") == "left",
        "gain_at_most_minus_24db": lambda event: float(event.get("gain_db", 0.0)) <= -24.0,
    }
    output: dict[str, dict[str, dict]] = {}
    for bucket, predicate in buckets.items():
        output[bucket] = {}
        for name in CLASS_NAMES:
            indices = [index for index, event in enumerate(truth)
                       if event["class"] == name and predicate(event)]
            hits = sum(index in matched_truth for index in indices)
            low, high = _wilson(hits, len(indices))
            output[bucket][name] = {
                "matched": hits, "support": len(indices),
                "recall": hits / max(1, len(indices)),
                "recall_ci95_low": low, "recall_ci95_high": high,
                "conclusive": len(indices) >= 30,
            }
    return output


def _error_clips(prefix: Path, audio: Audio, truth: list[dict], predictions: list[PredictedEvent]) -> None:
    pairs, matched_truth, matched_predictions = match_events(truth, predictions)
    del pairs
    clips = prefix / "errors"
    clips.mkdir(parents=True, exist_ok=True)
    stereo = to_stereo_48k(audio)
    context = SAMPLE_RATE
    errors: list[tuple[str, str, int, int]] = []
    for index, event in enumerate(truth):
        if index not in matched_truth and event["class"] in CLASS_NAMES:
            errors.append(("miss", event["class"], int(event["onset_sample"]), index))
    for index, event in enumerate(predictions):
        if index not in matched_predictions:
            errors.append(("false_positive", event.sound_class, event.onset_sample, index))
    for kind, name, center, index in errors:
        start = max(0, center - context)
        end = min(len(stereo.samples), center + context)
        write_pcm16_wav(clips / f"{kind}_{name}_{index:05d}.wav",
                        Audio(stereo.samples[start:end], SAMPLE_RATE))


def _overlap_case_rate(truth: list[dict], predictions: list[PredictedEvent]) -> dict:
    _, matched_truth, _ = match_events(truth, predictions)
    cases: list[tuple[int, int]] = []
    for gun_index, gun in enumerate(truth):
        if gun["class"] != "gunshot" or not bool(gun.get("overlap", False)):
            continue
        candidates = [
            (abs(int(foot["onset_sample"]) - int(gun["onset_sample"])), foot_index)
            for foot_index, foot in enumerate(truth)
            if foot["class"] == "footstep" and bool(foot.get("overlap", False))
            and abs(int(foot["onset_sample"]) - int(gun["onset_sample"])) <= 7200
        ]
        if candidates:
            cases.append((gun_index, min(candidates)[1]))
    passed = sum(gun in matched_truth and foot in matched_truth for gun, foot in cases)
    return {"passed": passed, "support": len(cases), "rate": passed / max(1, len(cases))}


def _rapid_separation(truth: list[dict], predictions: list[PredictedEvent]) -> dict[str, dict]:
    _, matched_truth, _ = match_events(truth, predictions)
    output: dict[str, dict] = {}
    for name, maximum_ms in (("gunshot", 60), ("footstep", 100)):
        indices = sorted(
            (index for index, event in enumerate(truth) if event["class"] == name),
            key=lambda index: int(truth[index]["onset_sample"]),
        )
        cases = [(first, second) for first, second in zip(indices, indices[1:])
                 if 0 < int(truth[second]["onset_sample"]) - int(truth[first]["onset_sample"])
                 <= maximum_ms * SAMPLE_RATE // 1000]
        passed = sum(first in matched_truth and second in matched_truth for first, second in cases)
        output[name] = {"passed": passed, "support": len(cases),
                        "rate": passed / max(1, len(cases)), "maximum_ms": maximum_ms}
    return output


def evaluate_sessions(sessions_dir: str | Path, package_dir: str | Path, output_dir: str | Path) -> dict:
    sessions = Path(sessions_dir)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    aggregate_truth: dict[str, list[dict]] = {"simple": [], "complex": [], "all": []}
    aggregate_predictions: dict[str, list[PredictedEvent]] = {"simple": [], "complex": [], "all": []}
    durations = {"simple": 0.0, "complex": 0.0, "all": 0.0}
    per_session: dict[str, dict] = {}
    offsets = {"simple": 0, "complex": 0, "all": 0}
    ambient_predictions: list[PredictedEvent] = []
    ambient_duration = 0.0
    gain_db_values = (0, -12, -24)
    gain_predictions = {gain_db: [] for gain_db in gain_db_values}
    gain_offsets = {gain_db: 0 for gain_db in gain_db_values}
    for wav_path in sorted(sessions.glob("test_*.wav")):
        prefix = wav_path.with_suffix("")
        timeline = load_timeline(prefix.with_suffix(".jsonl"))
        predictions, samples, probabilities = predict_wav(wav_path, package_dir)
        audio = to_stereo_48k(load_pcm_wav(wav_path))
        duration = len(audio.samples) / SAMPLE_RATE
        stratum = "simple" if "_simple_" in wav_path.name else "complex"
        metrics = calculate_metrics(timeline, predictions, duration)
        per_session[wav_path.stem] = {name: asdict(value) for name, value in metrics.items()}
        np.savez_compressed(output / f"{wav_path.stem}.scores.npz", samples=samples, probabilities=probabilities)
        _error_clips(output / wav_path.stem, audio, timeline, predictions)
        if not timeline:
            ambient_predictions.extend(predictions)
            ambient_duration += duration
        for gain_db in gain_db_values:
            if gain_db == 0:
                gain_events = predictions
            else:
                gain = np.float32(10.0 ** (gain_db / 20.0))
                gain_samples, gain_probabilities, gain_metadata = probabilities_for_audio(
                    Audio(audio.samples * gain, SAMPLE_RATE), package_dir
                )
                gain_events = postprocess(gain_samples, gain_probabilities, gain_metadata)
            offset = gain_offsets[gain_db]
            gain_predictions[gain_db].extend(
                PredictedEvent(event.sound_class, event.onset_sample + offset,
                               event.end_sample + offset, event.confidence,
                               event.detected_sample + offset if event.detected_sample else 0)
                for event in gain_events
            )
            gain_offsets[gain_db] += len(audio.samples)
        for bucket in (stratum, "all"):
            offset = offsets[bucket]
            aggregate_truth[bucket].extend({**event, "onset_sample": int(event["onset_sample"]) + offset,
                                            "end_sample": int(event["end_sample"]) + offset} for event in timeline)
            aggregate_predictions[bucket].extend(
                PredictedEvent(event.sound_class, event.onset_sample + offset, event.end_sample + offset,
                               event.confidence,
                               event.detected_sample + offset if event.detected_sample else 0)
                for event in predictions
            )
            offsets[bucket] += len(audio.samples)
            durations[bucket] += duration
    isolated_truth = [event for event in aggregate_truth["all"]
                      if event["class"] in ("gunshot", "footstep")
                      and not bool(event.get("overlap", False))]
    wrong_class_cotriggers = 0
    for event in isolated_truth:
        onset = int(event["onset_sample"])
        if any(prediction.sound_class != event["class"] and abs(prediction.onset_sample - onset) <= 7200
               for prediction in aggregate_predictions["all"]):
            wrong_class_cotriggers += 1
    strata = {
        bucket: {name: asdict(value) for name, value in
                 calculate_metrics(aggregate_truth[bucket], aggregate_predictions[bucket], durations[bucket]).items()}
        for bucket in ("simple", "complex", "all")
    }
    breakdowns = recall_breakdowns(aggregate_truth["all"], aggregate_predictions["all"])
    ambient_metrics = calculate_metrics([], ambient_predictions, ambient_duration)
    gain_recall = {
        str(gain_db): {name: metrics.recall for name, metrics in calculate_metrics(
            aggregate_truth["all"], gain_predictions[gain_db], durations["all"]).items()}
        for gain_db in gain_db_values
    }
    gain_delta = {
        name: max(gain_recall[str(gain_db)][name] for gain_db in gain_db_values) -
              min(gain_recall[str(gain_db)][name] for gain_db in gain_db_values)
        for name in CLASS_NAMES
    }
    overlap_cases = _overlap_case_rate(aggregate_truth["all"], aggregate_predictions["all"])
    rapid = _rapid_separation(aggregate_truth["all"], aggregate_predictions["all"])
    acceptance = {
        "wrong_class_cotrigger": wrong_class_cotriggers / max(1, len(isolated_truth)) < 0.01,
        "true_overlap": overlap_cases["support"] > 0 and overlap_cases["rate"] >= 0.90,
        "remote_footstep": breakdowns["remote_rendered"]["footstep"]["support"] > 0 and
                           breakdowns["remote_rendered"]["footstep"]["recall"] >= 0.90,
        "remote_footstep_quadrants": all(
            breakdowns[f"remote_{quadrant}"]["footstep"]["support"] > 0 and
            breakdowns[f"remote_{quadrant}"]["footstep"]["recall"] >= 0.85
            for quadrant in ("front", "right", "rear", "left")
        ),
        "gain_robustness": all(delta <= 0.05 for delta in gain_delta.values()),
        "simple_recall": strata["simple"]["gunshot"]["recall"] >= 1.0 and
                         strata["simple"]["footstep"]["recall"] >= 1.0,
        "complex_recall": strata["complex"]["gunshot"]["recall"] >= 0.95 and
                          strata["complex"]["footstep"]["recall"] >= 0.90 and
                          strata["complex"]["mechanical"]["recall"] >= 0.75,
        "ambient_false_alerts": ambient_duration > 0.0 and all(
            ambient_metrics[name].false_alerts_per_minute <= 0.1 for name in CLASS_NAMES
        ),
        "complex_false_alerts": sum(
            strata["complex"][name]["false_alerts_per_minute"] for name in CLASS_NAMES
        ) <= 2.0,
        "onset_error": all(
            strata["all"][name]["support"] > 0 and
            strata["all"][name]["median_absolute_onset_error_ms"] <= 30.0 and
            strata["all"][name]["p95_absolute_onset_error_ms"] <= 60.0
            for name in CLASS_NAMES
        ),
        "rapid_separation": all(values["support"] > 0 and values["rate"] >= 1.0
                                for values in rapid.values()),
    }
    report = {
        "report_version": "sound-eval-v3",
        "strata": {
            bucket: values for bucket, values in strata.items()
        },
        "durations_seconds": durations,
        "recall_breakdowns": breakdowns,
        "ambient": {name: asdict(value) for name, value in ambient_metrics.items()},
        "gain_recall": gain_recall,
        "gain_recall_max_delta": gain_delta,
        "true_overlap_cases": overlap_cases,
        "rapid_separation": rapid,
        "wrong_class_cotrigger_rate": wrong_class_cotriggers / max(1, len(isolated_truth)),
        "wrong_class_cotriggers": wrong_class_cotriggers,
        "isolated_target_support": len(isolated_truth),
        "acceptance": acceptance,
        "acceptance_passed": all(acceptance.values()),
        "sessions": per_session,
    }
    (output / "evaluation.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report
