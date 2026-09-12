"""Offline P0 evidence tools. No playback, training, or automatic gate approval.

All output paths are create-only. PCM16 RIFF/WAVE including extensible channel
masks is accepted; multichannel roles are never inferred from channel count.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np

ROLES = {1: "FL", 2: "FR", 4: "FC", 8: "LFE", 16: "BL", 32: "BR",
         512: "SL", 1024: "SR"}
CLASSES = ("footstep", "gunshot")


def read_json(path):
    def invalid(value):
        raise ValueError(f"nonfinite JSON number: {value}")
    return json.loads(Path(path).read_text(encoding="utf-8-sig"), parse_constant=invalid)


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_wav(path):
    """Preserve the declared mask; fail on ambiguous/truncated input."""
    raw = Path(path).read_bytes()
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("expected RIFF/WAVE")
    end = struct.unpack_from("<I", raw, 4)[0] + 8
    if end != len(raw):
        raise ValueError("RIFF length mismatch")
    chunks = {}
    offset = 12
    while offset < end:
        if offset + 8 > end:
            raise ValueError("truncated chunk header")
        name, size = struct.unpack_from("<4sI", raw, offset)
        offset += 8
        if offset + size + (size & 1) > end:
            raise ValueError("truncated chunk")
        if name in (b"fmt ", b"data"):
            if name in chunks:
                raise ValueError("duplicate audio chunk")
            chunks[name] = raw[offset:offset + size]
        offset += size + (size & 1)
    fmt, data = chunks.get(b"fmt ", b""), chunks.get(b"data", b"")
    if len(fmt) < 16 or not data:
        raise ValueError("missing format or audio data")
    tag, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", fmt)
    mask = 0
    if tag == 0xFFFE:
        if len(fmt) < 40 or struct.unpack_from("<H", fmt, 16)[0] < 22:
            raise ValueError("invalid extensible format")
        valid, mask = struct.unpack_from("<HI", fmt, 18)
        if valid != 16 or fmt[24:40] != bytes.fromhex("0100000000001000800000aa00389b71"):
            raise ValueError("expected extensible PCM16")
    elif tag != 1:
        raise ValueError("expected PCM16")
    if channels not in (1, 2, 6, 8) or bits != 16 or rate <= 0:
        raise ValueError("unsupported channels/rate/bit depth")
    if align != channels * 2 or byte_rate != rate * align or len(data) % align:
        raise ValueError("invalid PCM alignment")
    if mask and (mask.bit_count() != channels or mask & ~sum(ROLES)):
        raise ValueError("unsupported or inconsistent speaker mask")
    roles = [name for bit, name in ROLES.items() if mask & bit] if mask else []
    if not roles and channels == 2:
        roles = ["FL", "FR"]  # Conventional PCM stereo; not independent surround.
    samples = np.frombuffer(data, dtype="<i2").reshape(-1, channels).astype(np.float64) / 32768
    return samples, {"sample_rate": rate, "channels": channels, "channel_mask": mask,
                     "roles": roles, "frames": len(samples), "sha256": sha256(path)}


def audit(path):
    samples, metadata = read_wav(path)
    # Bound covariance workspace for long captures; rank is diagnostic, not proof.
    probe = samples[::max(1, len(samples) // 200000)]
    centered = probe - probe.mean(axis=0)
    gram = centered.T @ centered / len(centered)
    scale = np.sqrt(np.diag(gram))
    denominator = scale[:, None] * scale[None, :]
    correlation = np.divide(gram, denominator, out=np.zeros_like(gram), where=denominator > 1e-16)
    eigenvalues = np.maximum(np.linalg.eigvalsh(gram), 0)
    rank = int(np.sum(eigenvalues > max(1e-12, float(eigenvalues.max()) * 1e-5)))
    return {"schema": 1, "input": metadata,
            "rms": np.sqrt(np.mean(samples * samples, axis=0)).tolist(),
            "peak": np.max(np.abs(samples), axis=0).tolist(),
            "clipped_fraction": np.mean(np.abs(samples) >= 32767 / 32768, axis=0).tolist(),
            "correlation": correlation.tolist(), "observed_covariance_rank": rank,
            "spatial_validation": "unverified",
            "warning": "Channel count, nonzero energy and rank cannot establish native CS2 surround or direction accuracy."}


def number(value, name, minimum=0):
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or value < minimum:
        raise ValueError(f"invalid {name}")
    return float(value)


def validate_manifest(manifest, root):
    if manifest.get("schema") != 1 or not manifest.get("sessions"):
        raise ValueError("expected schema 1 and nonempty sessions")
    seen = set()
    split_keys = {"dev": {"maps": set(), "recordings": set(), "hashes": set()},
                  "test": {"maps": set(), "recordings": set(), "hashes": set()}}
    for session in manifest["sessions"]:
        sid = session["id"]
        if not isinstance(sid, str) or not sid or sid in seen:
            raise ValueError("duplicate or empty session id")
        seen.add(sid)
        if session.get("source") != "real-cs2" or session.get("reviewed") is not True:
            raise ValueError(f"{sid}: requires reviewed real CS2 capture, not generated audio")
        split = session["split"]
        if split not in split_keys:
            raise ValueError("split must be dev or test")
        for field in ("map", "recording_session", "endpoint", "audio_settings", "conditions"):
            if not session.get(field):
                raise ValueError(f"{sid}: missing {field}")
        audio, metadata = read_wav(root / session["wav"])
        if metadata["sample_rate"] != 48000:
            raise ValueError("P0 baseline requires recorded 48 kHz; no silent resampling")
        if session.get("sha256") != metadata["sha256"]:
            raise ValueError(f"{sid}: audio hash mismatch")
        if session.get("roles") != metadata["roles"] or not metadata["roles"]:
            raise ValueError(f"{sid}: declared roles do not match WAV")
        ids = set()
        for event in session["events"]:
            if event["id"] in ids or event.get("class") not in CLASSES:
                raise ValueError("duplicate event id or unsupported class")
            ids.add(event["id"])
            onset = number(event["onset_ms"], "onset_ms")
            if onset >= len(audio) / 48:
                raise ValueError("event outside recording")
            if not isinstance(event.get("localizable"), bool):
                raise ValueError("explicit localizable required")
            if event["localizable"] and number(event.get("azimuth_deg"), "azimuth_deg") >= 360:
                raise ValueError("azimuth must be in [0,360)")
        split_keys[split]["maps"].add(session["map"])
        split_keys[split]["recordings"].add(session["recording_session"])
        split_keys[split]["hashes"].add(metadata["sha256"])
    for key in ("maps", "recordings", "hashes"):
        if split_keys["dev"][key] & split_keys["test"][key]:
            raise ValueError(f"development/test leakage: {key}")
    return {"sessions": len(seen), "split_counts": {
        split: sum(s["split"] == split for s in manifest["sessions"]) for split in split_keys},
        "status": "manifest_consistent_not_accuracy_or_provenance_proof"}


def match_events(truth, predictions):
    """Maximum-cardinality class/onset bipartite matching, deterministic ties.

    Avoid nearest-pair greedy undercount in rapid overlapping sequences.
    Bearings are deliberately excluded from matching to prevent oracle choice.
    """
    edges = [sorted((j for j, p in enumerate(predictions)
                     if p["class"] == t["class"] and abs(p["onset_ms"] - t["onset_ms"]) <= 100),
                    key=lambda j: (abs(predictions[j]["onset_ms"] - t["onset_ms"]), j)) for t in truth]
    owners = {}
    def augment(i, visited):
        for j in edges[i]:
            if j in visited:
                continue
            visited.add(j)
            if j not in owners or augment(owners[j], visited):
                owners[j] = i
                return True
        return False
    for i in sorted(range(len(truth)), key=lambda i: (len(edges[i]), truth[i]["onset_ms"], i)):
        augment(i, set())
    return sorted((i, j) for j, i in owners.items())


def ratio(a, b):
    return a / b if b else None


def percentile(values, q):
    return float(np.percentile(values, q)) if values else None


def angle_error(a, b):
    return abs((a - b + 180) % 360 - 180)


def evaluate(sessions, predictions):
    known = {s["id"] for s in sessions}
    if set(predictions) != known:
        raise ValueError("predictions must include every evaluated session (empty arrays for no detections)")
    output = {}
    for cls in CLASSES:
        support = detected = matched_count = eligible = fb_eligible = fb_wrong = joint = 0
        errors, latencies = [], []
        for session in sessions:
            truth = [e for e in session["events"] if e["class"] == cls]
            predicted = [p for p in predictions[session["id"]] if p["class"] == cls]
            support += len(truth)
            detected += len(predicted)
            pairs = match_events(truth, predicted)
            matched_count += len(pairs)
            for i, j in pairs:
                event, prediction = truth[i], predicted[j]
                if prediction.get("hud_presented_ms") is not None:
                    latency = prediction["hud_presented_ms"] - event["onset_ms"]
                    if latency < 0:
                        raise ValueError("HUD precedes matched truth onset; review time alignment")
                    latencies.append(latency)
                if not event["localizable"]:
                    continue
                eligible += 1
                bearing = prediction.get("azimuth_deg")
                if bearing is None:
                    continue
                error = angle_error(bearing, event["azimuth_deg"])
                errors.append(error)
                joint += error <= 45
                # Exact lateral +/-5 degrees is excluded from front/rear labeling;
                # count and convention are exposed, not hidden in the headline.
                if min(angle_error(event["azimuth_deg"], 90), angle_error(event["azimuth_deg"], 270)) > 5:
                    fb_eligible += 1
                    fb_wrong += math.cos(math.radians(bearing)) * math.cos(math.radians(event["azimuth_deg"])) < 0
        output[cls] = {"truth": support, "tp": matched_count, "fp": detected - matched_count,
                       "fn": support - matched_count, "precision": ratio(matched_count, detected),
                       "recall": ratio(matched_count, support), "direction_eligible": eligible,
                       "direction_outputs": len(errors), "direction_coverage": ratio(len(errors), eligible),
                       "direction_median_deg": percentile(errors, 50), "direction_p90_deg": percentile(errors, 90),
                       "front_back_eligible": fb_eligible, "front_back_confusion": ratio(fb_wrong, fb_eligible),
                       "correct_class_and_direction_45_fraction_all_truth": ratio(joint, support),
                       "hud_latency_samples": len(latencies), "onset_to_hud_p95_ms": percentile(latencies, 95)}
    return {"schema": 1, "metrics": output,
            "unknown_predictions": sum(p["class"] == "unknown" for events in predictions.values() for p in events),
            "acceptance": "not_automatically_approved",
            "matching": "class + onset +/-100ms, maximum cardinality; no direction used for matching",
            "front_back_convention": "exclude truth within 5deg of 90/270; report denominator",
            "listening_latency": "not_measured_by_offline_evaluator"}


def validate_predictions(document, sessions):
    if document.get("schema") != 1 or not document.get("algorithm") or not document.get("configuration_sha256"):
        raise ValueError("predictions require schema, algorithm and configuration_sha256")
    digest = document["configuration_sha256"]
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        raise ValueError("configuration_sha256 must be a lowercase SHA-256 digest")
    predictions = document["sessions"]
    if set(predictions) != {s["id"] for s in sessions}:
        raise ValueError("prediction sessions do not match requested split")
    for sid, events in predictions.items():
        for event in events:
            if event.get("class") not in (*CLASSES, "unknown"):
                raise ValueError("invalid prediction class")
            onset = number(event["onset_ms"], "onset_ms")
            if event.get("azimuth_deg") is not None:
                if number(event["azimuth_deg"], "azimuth_deg") >= 360:
                    raise ValueError("invalid predicted bearing")
            elif not event.get("rejection_reason"):
                raise ValueError("direction abstention requires rejection_reason")
            for key in ("classification_confidence", "direction_confidence"):
                if key == "classification_confidence" and event.get(key) is None and event.get("confidence_kind") == "uncalibrated_rule_response":
                    continue  # A hard rule is not a calibrated probability.
                if number(event[key], key) > 1:
                    raise ValueError("confidence outside [0,1]")
            if event.get("hud_presented_ms") is not None:
                if number(event["hud_presented_ms"], "hud_presented_ms") < onset:
                    raise ValueError("HUD cannot precede predicted onset")
    return predictions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    scan = sub.add_parser("audit")
    scan.add_argument("wav", type=Path)
    check = sub.add_parser("validate")
    check.add_argument("manifest", type=Path)
    score = sub.add_parser("evaluate")
    score.add_argument("manifest", type=Path)
    score.add_argument("predictions", type=Path)
    score.add_argument("--split", choices=["dev", "test"], default="test")
    for child in (scan, check, score):
        child.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "audit":
        result = audit(args.wav)
    else:
        manifest = read_json(args.manifest)
        result = validate_manifest(manifest, args.manifest.parent)
        if args.command == "evaluate":
            sessions = [s for s in manifest["sessions"] if s["split"] == args.split]
            if not sessions:
                raise ValueError("no sessions in requested split")
            document = read_json(args.predictions)
            predictions = validate_predictions(document, sessions)
            result = evaluate(sessions, predictions)
            result.update({"manifest_sha256": sha256(args.manifest), "predictions_sha256": sha256(args.predictions),
                           "algorithm": document["algorithm"], "configuration_sha256": document["configuration_sha256"],
                           "split": args.split})
    write_json(args.output, result)


if __name__ == "__main__":
    main()
