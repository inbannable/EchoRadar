import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from echoradar_ml import (
    CLASS_NAMES, CONTEXT_FRAMES, FFT_SIZE, HOP_SIZE, INFERENCE_STRIDE_FRAMES,
    INPUT_CHANNELS, MEL_BINS, PCEN_ALPHA, PCEN_DELTA, PCEN_EPSILON, PCEN_ROOT,
    PCEN_SMOOTHING, SAMPLE_RATE, SOURCE_NAMES, V4_PREPROCESSING_VERSION,
)
from echoradar_ml.inference import load_package


class InferencePackageTest(unittest.TestCase):
    def test_v4_package_contract_and_checksum(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "recognizer.onnx"
            model.write_bytes(b"test model bytes")
            metadata = {
                "package_version": 4,
                "model_version": "test-v4",
                "model_file": model.name,
                "model_sha256": hashlib.sha256(model.read_bytes()).hexdigest(),
                "preprocessing_version": V4_PREPROCESSING_VERSION,
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
                "pcen_smoothing": PCEN_SMOOTHING,
                "pcen_alpha": PCEN_ALPHA,
                "pcen_delta": PCEN_DELTA,
                "pcen_root": PCEN_ROOT,
                "pcen_epsilon": PCEN_EPSILON,
                "scene_activity_cutoff": 0.4,
                "self_suppression_threshold": 0.95,
                "peak_lookahead_frames": 2,
            }
            for name in CLASS_NAMES:
                metadata[f"threshold_quiet_{name}"] = 0.5
                metadata[f"threshold_busy_{name}"] = 0.7
                metadata[f"minimum_spacing_ms_{name}"] = 35 if name == "gunshot" else 80
                metadata[f"onset_offset_samples_{name}"] = 500
            (root / "model.json").write_text(json.dumps(metadata), encoding="utf-8")
            loaded, path = load_package(root)
            self.assertEqual(loaded["class_order"], "gunshot,footstep")
            self.assertEqual(path, model)
            model.write_bytes(b"corrupted")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                load_package(root)


if __name__ == "__main__":
    unittest.main()
