"""Controlled P0 ablation: frequency-only versus frequency + time rules.

Requires an explicit development-set configuration; ships no semantic band
presets or classification confidence. Does not associate mixture peaks with
event classes. Frozen legacy ONNX comparison remains a separate algorithm.
"""
import argparse
from pathlib import Path

import numpy as np

from features import BANDS, extract
from p0 import CLASSES, read_json, read_wav, sha256, validate_manifest, write_json


def validate_config(config):
    if config.get("schema") != 1 or config.get("calibrated_on") != "dev":
        raise ValueError("rules require schema 1 and calibrated_on=dev")
    if not config.get("development_manifest_sha256") or not config.get("calibration_notes"):
        raise ValueError("record development manifest hash and calibration notes")
    digest = config["development_manifest_sha256"]
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        raise ValueError("invalid development manifest hash")
    if set(config.get("rules", {})) != set(CLASSES):
        raise ValueError("both class rules are required")
    for rule in config["rules"].values():
        selected = rule.get("band_indices", [])
        if not selected or len(selected) != len(set(selected)) or any(type(i) is not int or i not in range(len(BANDS)) for i in selected):
            raise ValueError("select valid unique band indices")
        for key in ("minimum_energy", "minimum_snr_db", "minimum_flux_ratio", "minimum_duration_ms",
                    "maximum_duration_ms", "repeat_min_ms", "repeat_max_ms"):
            value = rule.get(key)
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not np.isfinite(value) or value < 0:
                raise ValueError(f"invalid rule parameter: {key}")
        if rule["maximum_duration_ms"] < rule["minimum_duration_ms"] or rule["repeat_max_ms"] < rule["repeat_min_ms"]:
            raise ValueError("invalid temporal range")
        if type(rule.get("minimum_repeats")) is not int or rule["minimum_repeats"] < 1:
            raise ValueError("minimum_repeats must be a positive integer")


def detect(rows, config, mode):
    """Causal rule triggers; time mode waits for minimum duration/repetitions.

    Frequency mode emits once at each above-threshold run. Time mode adds
    onset flux, bounded duration and optional repetition. No future samples
    or truth labels enter detection. Truncated final runs remain unclassified
    in time mode because their duration is not yet known.
    """
    if mode not in ("frequency", "frequency-time"):
        raise ValueError("invalid baseline mode")
    validate_config(config)
    output = []
    for cls, rule in config["rules"].items():
        active = None
        previous_onset = None
        repeats = 0
        selected = rule["band_indices"]
        for row in rows:
            energy = float(np.mean(np.array(row["band_energy"])[selected]))
            snr = float(np.mean(np.array(row["band_snr_db"])[selected]))
            above = energy >= rule["minimum_energy"] and snr >= rule["minimum_snr_db"]
            emit = False
            candidate = None
            if above and active is None:
                flux = float(np.mean(np.array(row["positive_energy_flux"])[selected])) / max(energy, 1e-12)
                active = {"onset_ms": row["window_start_ms"], "start_available_ms": row["available_at_ms"], "flux": flux}
                if mode == "frequency":
                    candidate, emit = active, True
            elif not above and active is not None:
                candidate = active
                duration = row["available_at_ms"] - active["start_available_ms"]
                if mode == "frequency-time" and rule["minimum_duration_ms"] <= duration <= rule["maximum_duration_ms"] and active["flux"] >= rule["minimum_flux_ratio"]:
                    interval = active["onset_ms"] - previous_onset if previous_onset is not None else None
                    repeats = repeats + 1 if interval is not None and rule["repeat_min_ms"] <= interval <= rule["repeat_max_ms"] else 1
                    previous_onset = active["onset_ms"]
                    emit = repeats >= rule["minimum_repeats"]
                active = None
            if emit:
                output.append({"class": cls, "onset_ms": candidate["onset_ms"],
                               "algorithm_available_at_ms": row["available_at_ms"],
                               "classification_confidence": None, "confidence_kind": "uncalibrated_rule_response",
                               "direction_confidence": 0, "azimuth_deg": None,
                               "rejection_reason": "event_direction_association_not_validated",
                               "hud_presented_ms": None})
    return sorted(output, key=lambda event: (event["onset_ms"], event["class"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("configuration", type=Path)
    parser.add_argument("--mode", choices=["frequency", "frequency-time"], required=True)
    parser.add_argument("--split", choices=["dev", "test"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest, config = read_json(args.manifest), read_json(args.configuration)
    validate_manifest(manifest, args.manifest.parent)
    validate_config(config)
    sessions = [s for s in manifest["sessions"] if s["split"] == args.split]
    if not sessions:
        raise ValueError("empty requested split")
    output = {}
    for session in sessions:
        samples, metadata = read_wav(args.manifest.parent / session["wav"])
        rows = extract(samples, metadata["roles"], metadata["sample_rate"])
        output[session["id"]] = detect(rows, config, args.mode)
    write_json(args.output, {"schema": 1, "algorithm": f"echoradar-p0-{args.mode}-v1",
                            "configuration_sha256": sha256(args.configuration), "sessions": output,
                            "status": "research_ablation_not_accepted_product"})


if __name__ == "__main__":
    main()
