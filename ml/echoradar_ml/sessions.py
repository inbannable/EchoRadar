from __future__ import annotations

import json
from pathlib import Path
import re

from . import CLASS_NAMES, SAMPLE_RATE, SOURCE_NAMES
from .audio import load_pcm_wav, to_stereo_48k, write_pcm16_wav


SESSION_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$")


def load_timeline(path: str | Path) -> list[dict]:
    events: list[dict] = []
    for line_number, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError as error:
            raise ValueError(f"invalid JSONL at {path}:{line_number}: {error}") from error
    return events


def validate_timeline(events: list[dict], frame_count: int) -> list[dict]:
    normalized: list[dict] = []
    for index, event in enumerate(events):
        name = str(event.get("class", ""))
        if name not in (*CLASS_NAMES, "negative"):
            raise ValueError(f"event {index} has unsupported class {name!r}")
        onset = int(event.get("onset_sample", -1))
        end = int(event.get("end_sample", onset + SAMPLE_RATE // 20))
        if not (0 <= onset < frame_count and onset < end <= frame_count):
            raise ValueError(f"event {index} has invalid sample bounds {onset}:{end}")
        uncertain = bool(event.get("uncertain", False))
        source = str(event.get("source_hint", event.get("source", "unknown")))
        if source == "manual" or source == "model":
            source = "unknown"
        if source not in SOURCE_NAMES:
            raise ValueError(f"event {index} has invalid source_hint {source!r}")
        normalized.append({
            **event,
            "class": name,
            "onset_sample": onset,
            "end_sample": end,
            "source_hint": source,
            "reviewed": bool(event.get("reviewed", True)),
            "uncertain": uncertain,
            "scene_mode": str(event.get("scene_mode", "unknown")),
        })
    normalized.sort(key=lambda event: (int(event["onset_sample"]), event["class"]))
    return normalized


def write_timeline(path: str | Path, events: list[dict]) -> None:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        for event in events:
            stream.write(json.dumps(event, sort_keys=True) + "\n")
    temporary.replace(output)


def import_real_session(
    wav_path: str | Path,
    labels_path: str | Path,
    output_dir: str | Path,
    split: str,
    session_id: str,
    map_name: str,
    capture_day: str,
    audio_settings: str,
    label_sample_rate: int = SAMPLE_RATE,
) -> Path:
    """Normalize one fully reviewed gameplay recording into the corpus layout."""
    if split not in ("train", "dev", "test"):
        raise ValueError("split must be train, dev, or test")
    if not SESSION_ID.fullmatch(session_id):
        raise ValueError("session_id must be a short filesystem-safe identifier")
    if label_sample_rate <= 0:
        raise ValueError("label_sample_rate must be positive")
    audio = to_stereo_48k(load_pcm_wav(wav_path))
    raw_events = load_timeline(labels_path)
    if label_sample_rate != SAMPLE_RATE:
        ratio = SAMPLE_RATE / label_sample_rate
        raw_events = [{
            **event,
            "onset_sample": int(round(int(event["onset_sample"]) * ratio)),
            "end_sample": int(round(int(event.get(
                "end_sample", int(event["onset_sample"]) + label_sample_rate // 20
            )) * ratio)),
        } for event in raw_events]
    events = validate_timeline(raw_events, len(audio.samples))
    if any(not event["reviewed"] for event in events):
        raise ValueError("real-session import requires every retained label to be reviewed")

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    prefix = output / f"{split}_real_{session_id}"
    if any(prefix.with_suffix(suffix).exists() for suffix in (".wav", ".jsonl", ".session.json")):
        raise FileExistsError(f"session already exists: {prefix}")
    write_pcm16_wav(prefix.with_suffix(".wav"), audio)
    write_timeline(prefix.with_suffix(".jsonl"), events)
    metadata = {
        "generator_version": "real-session-v4",
        "session_id": session_id,
        "split": split,
        "sample_rate": SAMPLE_RATE,
        "channels": 2,
        "duration_seconds": len(audio.samples) / SAMPLE_RATE,
        "map": map_name,
        "capture_day": capture_day,
        "audio_settings": audio_settings,
        "label_sample_rate": label_sample_rate,
        "source_wav": str(Path(wav_path).resolve()),
        "source_labels": str(Path(labels_path).resolve()),
    }
    prefix.with_suffix(".session.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return prefix


def audit_session_corpus(sessions_dir: str | Path) -> dict:
    root = Path(sessions_dir)
    report: dict[str, object] = {
        "sessions": 0,
        "duration_seconds": 0.0,
        "target_free_seconds": 0.0,
        "test_target_free_seconds": 0.0,
        "maps": [],
        "capture_days": [],
        "test_maps": [],
        "test_capture_days": [],
        "support": {},
        "uncertain_events": 0,
        "unreviewed_events": 0,
    }
    support: dict[str, int] = {}
    maps: set[str] = set()
    days: set[str] = set()
    test_maps: set[str] = set()
    test_days: set[str] = set()
    for wav_path in sorted(root.glob("*.wav")):
        prefix = wav_path.with_suffix("")
        labels_path = prefix.with_suffix(".jsonl")
        metadata_path = prefix.with_suffix(".session.json")
        if not labels_path.exists() or not metadata_path.exists():
            continue
        audio = to_stereo_48k(load_pcm_wav(wav_path))
        events = validate_timeline(load_timeline(labels_path), len(audio.samples))
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        split = str(metadata.get("split", wav_path.stem.split("_", 1)[0]))
        duration = len(audio.samples) / SAMPLE_RATE
        report["sessions"] = int(report["sessions"]) + 1
        report["duration_seconds"] = float(report["duration_seconds"]) + duration
        target_events = [event for event in events if event["class"] in CLASS_NAMES and not event["uncertain"]]
        if not target_events:
            report["target_free_seconds"] = float(report["target_free_seconds"]) + duration
            if split == "test":
                report["test_target_free_seconds"] = float(report["test_target_free_seconds"]) + duration
        for event in events:
            report["uncertain_events"] = int(report["uncertain_events"]) + int(event["uncertain"])
            report["unreviewed_events"] = int(report["unreviewed_events"]) + int(not event["reviewed"])
            if event["class"] in CLASS_NAMES and not event["uncertain"]:
                key = f"{split}:{event['class']}:{event['source_hint']}"
                support[key] = support.get(key, 0) + 1
        if metadata.get("map"):
            maps.add(str(metadata["map"]))
            if split == "test":
                test_maps.add(str(metadata["map"]))
        if metadata.get("capture_day"):
            days.add(str(metadata["capture_day"]))
            if split == "test":
                test_days.add(str(metadata["capture_day"]))
    report["maps"] = sorted(maps)
    report["capture_days"] = sorted(days)
    report["test_maps"] = sorted(test_maps)
    report["test_capture_days"] = sorted(test_days)
    report["support"] = dict(sorted(support.items()))
    return report


def training_readiness(report: dict) -> dict[str, bool]:
    support = report.get("support", {})
    splits_present = {
        key.split(":", 1)[0] for key, value in support.items() if int(value) > 0
    }
    return {
        "train_and_dev_present": {"train", "dev"}.issubset(splits_present),
        "all_labels_reviewed": int(report.get("unreviewed_events", 0)) == 0,
        "locked_remote_support": all(
            int(support.get(f"test:{name}:remote", 0)) >= 300 for name in CLASS_NAMES
        ),
        "locked_self_support": all(
            int(support.get(f"test:{name}:self", 0)) >= 200 for name in CLASS_NAMES
        ),
        "target_free_hour": float(report.get("test_target_free_seconds", 0.0)) >= 3600.0,
        "map_diversity": len(report.get("test_maps", [])) >= 3,
        "capture_day_diversity": len(report.get("test_capture_days", [])) >= 3,
    }
