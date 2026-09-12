import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from p0 import (audit, evaluate, match_events, read_wav, sha256,
                validate_manifest, validate_predictions, write_json)
from features import direction_activity, extract
from baselines import detect, validate_config


def wav(path, values):
    with wave.open(str(path), "wb") as stream:
        stream.setparams((values.shape[1], 2, 48000, 0, "NONE", "not compressed"))
        stream.writeframes(values.astype("<i2").tobytes())


class EvidenceTests(unittest.TestCase):
    def test_maximum_matching_avoids_greedy_loss(self):
        truth = [{"class": "footstep", "onset_ms": n} for n in (100, 200)]
        prediction = [{"class": "footstep", "onset_ms": n} for n in (10, 110)]
        self.assertEqual(len(match_events(truth, prediction)), 2)

    def test_duplicate_predictions_are_false_positive_and_abstention_costs_coverage(self):
        session = {"id": "s", "events": [
            {"class": "footstep", "onset_ms": 100, "localizable": True, "azimuth_deg": 359},
            {"class": "footstep", "onset_ms": 400, "localizable": True, "azimuth_deg": 180}]}
        predictions = {"s": [{"class": "footstep", "onset_ms": 100, "azimuth_deg": 1},
                             {"class": "footstep", "onset_ms": 101, "azimuth_deg": 180},
                             {"class": "footstep", "onset_ms": 400, "azimuth_deg": None}]}
        result = evaluate([session], predictions)["metrics"]["footstep"]
        self.assertEqual((result["tp"], result["fp"], result["fn"]), (2, 1, 0))
        self.assertEqual(result["direction_median_deg"], 2)
        self.assertEqual(result["direction_coverage"], .5)
        self.assertEqual(result["correct_class_and_direction_45_fraction_all_truth"], .5)
        self.assertIsNone(result["onset_to_hud_p95_ms"])

    def test_empty_predictions_keep_false_negatives_and_missing_measurements(self):
        session = {"id": "s", "events": [{"class": "gunshot", "onset_ms": 100, "localizable": False}]}
        result = evaluate([session], {"s": []})["metrics"]["gunshot"]
        self.assertEqual(result["fn"], 1)
        self.assertEqual(result["recall"], 0)
        self.assertIsNone(result["precision"])
        self.assertIsNone(result["direction_coverage"])
        with self.assertRaises(ValueError):
            evaluate([session], {})

    def test_manifest_rejects_generated_and_map_leakage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wav(root / "a.wav", np.zeros((48000, 2)))
            a = {"id": "a", "source": "real-cs2", "reviewed": True, "split": "dev",
                 "map": "map1", "recording_session": "day1", "endpoint": "test",
                 "audio_settings": {"spatial": "off"}, "conditions": ["quiet"],
                 "wav": "a.wav", "sha256": sha256(root / "a.wav"), "roles": ["FL", "FR"], "events": []}
            manifest = {"schema": 1, "sessions": [a]}
            self.assertEqual(validate_manifest(manifest, root)["sessions"], 1)
            a["source"] = "generated"
            with self.assertRaisesRegex(ValueError, "real CS2"):
                validate_manifest(manifest, root)
            a["source"] = "real-cs2"
            b = copy.deepcopy(a)
            b.update(id="b", split="test", recording_session="day2")
            manifest["sessions"].append(b)
            with self.assertRaisesRegex(ValueError, "leakage"):
                validate_manifest(manifest, root)

    def test_silence_rank_is_zero_and_never_proves_surround(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "silence.wav"
            wav(path, np.zeros((1024, 8)))
            result = audit(path)
            self.assertEqual(result["observed_covariance_rank"], 0)
            self.assertEqual(result["input"]["roles"], [])
            self.assertEqual(result["spatial_validation"], "unverified")
            raw = path.read_bytes()
            path.write_bytes(raw[:-1])
            with self.assertRaises(ValueError):
                read_wav(path)

    def test_extensible_mask_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            fmt = struct.pack("<HHIIHHHHI", 65534, 8, 48000, 768000, 16, 16, 22, 16, 0x63f)
            fmt += bytes.fromhex("0100000000001000800000aa00389b71")
            body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", 32) + bytes(32)
            path = Path(directory) / "eight.wav"
            path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
            self.assertEqual(read_wav(path)[1]["roles"], ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"])

    def test_outputs_cannot_overwrite_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "existing.json"
            write_json(path, {"preserve": True})
            with self.assertRaises(FileExistsError):
                write_json(path, {})
            self.assertEqual(json.loads(path.read_text()), {"preserve": True})

    def test_misaligned_latency_cannot_improve_p95(self):
        session = {"id": "s", "events": [{"class": "gunshot", "onset_ms": 100, "localizable": False}]}
        with self.assertRaisesRegex(ValueError, "time alignment"):
            evaluate([session], {"s": [{"class": "gunshot", "onset_ms": 90, "hud_presented_ms": 95}]})

    def test_predictions_reject_nonfinite_confidence_and_missing_rejection(self):
        doc = {"schema": 1, "algorithm": "test", "configuration_sha256": "a" * 64,
               "sessions": {"s": [{"class": "gunshot", "onset_ms": 100, "azimuth_deg": None,
                                    "classification_confidence": .5, "direction_confidence": 0}]}}
        with self.assertRaisesRegex(ValueError, "rejection_reason"):
            validate_predictions(doc, [{"id": "s"}])
        event = doc["sessions"]["s"][0]
        event["rejection_reason"] = "unverified"
        event["classification_confidence"] = float("nan")
        with self.assertRaisesRegex(ValueError, "confidence"):
            validate_predictions(doc, [{"id": "s"}])


class ProbeTests(unittest.TestCase):
    roles = ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"]

    def test_lfe_ignored_and_stereo_rejected(self):
        spectrum = np.zeros((513, 8), complex)
        spectrum[:, 3] = 10
        self.assertEqual(direction_activity(spectrum, self.roles)["peaks"], [])
        self.assertIsNone(direction_activity(spectrum[:, :2], ["FL", "FR"]))

    def test_opposite_different_frequencies_keep_two_peaks(self):
        spectrum = np.zeros((513, 8), complex)
        spectrum[10, 6] = 1
        spectrum[20, 7] = 1
        peaks = direction_activity(spectrum, self.roles)["peaks"]
        self.assertEqual({p["azimuth_deg"] for p in peaks}, {90, 270})
        spectrum[10, 7] = 1
        spectrum[20, 7] = 0
        self.assertEqual(direction_activity(spectrum, self.roles)["peaks"], [])

    def test_stereo_silence_abstains_and_prefix_is_causal(self):
        rng = np.random.default_rng(7)
        samples = rng.normal(0, .01, (4096, 2))
        short = extract(samples[:2048], ["FL", "FR"])
        long = extract(samples, ["FL", "FR"])
        self.assertEqual(short, long[:len(short)])
        silence = extract(np.zeros((2048, 2)), ["FL", "FR"])
        self.assertIsNone(silence[0]["binaural_features"][0]["lag_left_minus_right_us"])
        self.assertIsNone(silence[0]["direction_activity"])
        self.assertIn("no_calibrated", silence[0]["direction_rejection"])


class BaselineTests(unittest.TestCase):
    def test_time_ablation_rejects_short_transient_without_inventing_confidence(self):
        rule = {"band_indices": [0], "minimum_energy": .2, "minimum_snr_db": 8,
                "minimum_flux_ratio": .5, "minimum_duration_ms": 20, "maximum_duration_ms": 50,
                "minimum_repeats": 1, "repeat_min_ms": 100, "repeat_max_ms": 700}
        config = {"schema": 1, "calibrated_on": "dev", "development_manifest_sha256": "a" * 64,
                  "calibration_notes": "synthetic unit-test fixture, not CS2 tuning",
                  "rules": {"footstep": rule, "gunshot": {**rule, "minimum_energy": 10}}}
        rows = [{"window_start_ms": i * 10, "available_at_ms": i * 10 + 22,
                 "band_energy": [energy] * 6, "band_snr_db": [20 if energy else 0] * 6,
                 "positive_energy_flux": [energy] * 6}
                for i, energy in enumerate([0, 1, 1, 1, 0, 1, 0])]
        pure, temporal = detect(rows, config, "frequency"), detect(rows, config, "frequency-time")
        self.assertEqual(len(pure), 2)
        self.assertEqual(len(temporal), 1)
        self.assertIsNone(temporal[0]["classification_confidence"])
        self.assertIsNone(temporal[0]["azimuth_deg"])
        self.assertEqual(temporal[0]["algorithm_available_at_ms"], 62)
        self.assertEqual(detect(rows[:4], config, "frequency-time"), [])
        validate_predictions({"schema": 1, "algorithm": "unit", "configuration_sha256": "b" * 64,
                              "sessions": {"s": temporal}}, [{"id": "s"}])
        config["calibrated_on"] = "test"
        with self.assertRaises(ValueError):
            validate_config(config)


if __name__ == "__main__":
    unittest.main()
