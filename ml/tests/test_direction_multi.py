from dataclasses import asdict, replace
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

import numpy as np

from echoradar_ml import SAMPLE_RATE
from echoradar_ml.audio import Audio, write_pcm16_wav
from echoradar_ml.direction_scenes import (
    DIRECTION_SCENE_FRAMES,
    DIRECTION_SCENE_SAMPLES,
    DirectionSceneSource,
    SceneDirection,
    generate_direction_scene,
    great_circle_degrees,
    sample_scene_directions,
    target_class_schedule,
    target_count_schedule,
)
from echoradar_ml.direction_training import (
    DIRECTION_OUTPUT_SHAPE,
    classwise_adpit_mse,
    build_direction_model,
    decode_multi_accdoa,
    direction_vector,
    evaluate_direction_outputs,
    load_direction_package,
    prepare_direction_cache,
)
from echoradar_ml.manifest import Asset


TORCH_AVAILABLE = importlib.util.find_spec("torch") is not None


class _FakeRenderer:
    def render(self, samples: np.ndarray, parameters) -> np.ndarray:
        mono = samples.astype(np.float32).mean(axis=1)
        output = np.stack((mono, mono), axis=1)
        pan = np.sin(np.deg2rad(parameters.azimuth_degrees))
        output[:, 0] *= np.float32(1.0 - 0.25 * pan)
        output[:, 1] *= np.float32(1.0 + 0.25 * pan)
        return output


def _write_asset(root: Path, relative: str, frequency: float) -> None:
    time = np.arange(7000, dtype=np.float32) / SAMPLE_RATE
    samples = np.zeros((7000, 1), dtype=np.float32)
    samples[300:, 0] = 0.2 * np.sin(2.0 * np.pi * frequency * time[:-300])
    write_pcm16_wav(root / relative, Audio(samples, SAMPLE_RATE))


class DirectionSceneTest(unittest.TestCase):
    def test_scene_contract_count_proportions_and_separation(self):
        self.assertEqual(DIRECTION_SCENE_SAMPLES, 1024 + 47 * 240)
        self.assertEqual(DIRECTION_SCENE_FRAMES, 48)
        schedule = target_count_schedule(100, 17)
        self.assertEqual([schedule.count(value) for value in range(4)], [10, 15, 35, 40])
        combinations = target_class_schedule([1] * 20 + [2] * 30 + [3] * 40, 17)
        one_source = combinations[:20]
        two_source = combinations[20:50]
        three_source = combinations[50:]
        self.assertEqual({value: one_source.count(value) for value in set(one_source)},
                         {("footstep",): 10, ("gunshot",): 10})
        self.assertEqual(set(two_source), {
            ("footstep", "footstep"), ("gunshot", "gunshot"),
            ("footstep", "gunshot"),
        })
        self.assertTrue(all(two_source.count(value) == 10 for value in set(two_source)))
        self.assertEqual(len(set(three_source)), 4)
        self.assertTrue(all(three_source.count(value) == 10 for value in set(three_source)))
        rng = __import__("random").Random(99)
        directions = sample_scene_directions(rng, 3, close_source=True)
        distances = [
            great_circle_degrees(directions[left], directions[right])
            for left in range(3) for right in range(left + 1, 3)
        ]
        self.assertGreaterEqual(min(distances), 15.0 - 1e-7)
        self.assertTrue(any(distance <= 30.0 + 1e-7 for distance in distances))
        self.assertTrue(all(-60.0 <= item.elevation_degrees <= 60.0 for item in directions))

    def test_scene_render_is_deterministic_and_only_final_gain_prevents_clipping(self):
        from tempfile import TemporaryDirectory

        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_asset(root, "gunshot.wav", 700.0)
            _write_asset(root, "footstep.wav", 230.0)
            _write_asset(root, "reload.wav", 1100.0)
            assets = [
                Asset("g", "gunshot.wav", "gunshot", "weapon:ak", "g", "", True,
                      "test", active_offset_samples=300),
                Asset("f", "footstep.wav", "footstep", "surface:stone", "f", "", True,
                      "test", active_offset_samples=300),
                Asset("n", "reload.wav", "negative", "weapon:reload", "n", "", True,
                      "test", subtype="reload", active_offset_samples=300),
            ]
            first_audio, first_row = generate_direction_scene(
                assets, root, "test", 4, 123, ("footstep", "gunshot", "footstep"),
                _FakeRenderer(), close_source=True,
            )
            second_audio, second_row = generate_direction_scene(
                assets, root, "test", 4, 123, ("footstep", "gunshot", "footstep"),
                _FakeRenderer(), close_source=True,
            )
            np.testing.assert_array_equal(first_audio.samples, second_audio.samples)
            self.assertEqual(asdict(first_row), asdict(second_row))
            self.assertEqual(first_audio.samples.shape, (DIRECTION_SCENE_SAMPLES, 2))
            self.assertLessEqual(float(np.abs(first_audio.samples).max()), 0.980001)
            self.assertEqual(first_row.target_count, 3)
            self.assertTrue(all(target.sound_class in ("gunshot", "footstep")
                                for target in first_row.targets))
            self.assertTrue(all(distractor.negative_subtype == "reload"
                                for distractor in first_row.distractors))

            jittered_audio, jittered_row = generate_direction_scene(
                assets, root, "test", 4, 123, ("footstep", "gunshot", "footstep"),
                _FakeRenderer(), close_source=True, coincident_onsets=False,
            )
            self.assertFalse(jittered_row.coincident_onsets)
            self.assertEqual(jittered_audio.samples.shape, first_audio.samples.shape)
            self.assertTrue(all(
                target.onset_sample > jittered_row.targets[0].onset_sample
                for target in jittered_row.targets[1:]
            ))

            scene_path = root / "scene.wav"
            write_pcm16_wav(scene_path, first_audio)
            persisted_row = replace(
                first_row,
                relative_path=scene_path.name,
                wav_sha256=hashlib.sha256(scene_path.read_bytes()).hexdigest(),
            )
            manifest = root / "direction-scenes.jsonl"
            manifest.write_text(
                json.dumps(persisted_row.json_dict(), sort_keys=True) + "\n",
                encoding="utf-8",
            )
            cache = prepare_direction_cache(
                manifest, root, root / "cache", splits=("test",), feature_dtype="float32"
            )
            cached = np.load(cache / "test" / "features.npy", allow_pickle=False)
            self.assertEqual(cached.shape, (1, 5, 48, 64))


class DirectionPostProcessingTest(unittest.TestCase):
    def test_direction_package_checksum_and_uncertainty_contract(self):
        from tempfile import TemporaryDirectory

        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            model = root / "direction.onnx"
            model.write_bytes(b"direction model")
            metadata = {
                "package_version": 1,
                "model_version": "test-v1",
                "model_file": model.name,
                "model_sha256": hashlib.sha256(model.read_bytes()).hexdigest(),
                "preprocessing_version": "stereo-onset-v4-scene48",
                "sample_rate": 48000,
                "fft_size": 1024,
                "hop_size": 240,
                "mel_bins": 64,
                "context_frames": 48,
                "context_samples": 12304,
                "input_channels": 5,
                "class_order": "gunshot,footstep",
                "track_order": "exchangeable-0,exchangeable-1,exchangeable-2",
                "track_count": 3,
                "coordinate_system": "x-right,y-up,z-forward",
                "maximum_sources": 3,
                "threshold_gunshot": 0.4,
                "threshold_footstep": 0.35,
                "elevation_min_degrees": -60.0,
                "elevation_max_degrees": 60.0,
                "duplicate_merge_degrees": 7.5,
                "minimum_training_separation_degrees": 15.0,
                "pcen_smoothing": 0.025,
                "pcen_alpha": 0.98,
                "pcen_delta": 2.0,
                "pcen_root": 0.5,
                "pcen_epsilon": 1.0e-6,
                "uncertainty_count": 2,
                "uncertainty_confidence_0": 0.0,
                "uncertainty_p90_degrees_0": 60.0,
                "uncertainty_confidence_1": 1.0,
                "uncertainty_p90_degrees_1": 10.0,
            }
            (root / "direction.json").write_text(json.dumps(metadata), encoding="utf-8")
            loaded, path = load_direction_package(root)
            self.assertEqual(loaded["track_count"], 3)
            self.assertEqual(path, model)
            model.write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                load_direction_package(root)

    def test_duplicate_merge_preserves_valid_same_class_sources_and_filters_gunshots(self):
        output = np.zeros(DIRECTION_OUTPUT_SHAPE, dtype=np.float32)
        output[0, 0] = direction_vector(90.0, 0.0) * 0.95
        output[1, 0] = direction_vector(0.0, 10.0) * 0.90
        output[1, 1] = direction_vector(3.0, 10.0) * 0.80
        output[1, 2] = direction_vector(15.0, -5.0) * 0.85
        decoded = decode_multi_accdoa(
            output, (0.3, 0.3), enabled_classes=(False, True)
        )
        self.assertEqual(len(decoded), 2)
        self.assertTrue(all(item.sound_class == "footstep" for item in decoded))
        self.assertTrue(any(abs(item.azimuth_degrees - 15.0) < 0.1 for item in decoded))

        signed = np.zeros(DIRECTION_OUTPUT_SHAPE, dtype=np.float32)
        signed[1, 0] = direction_vector(30.0, 35.0) * 0.9
        signed[1, 1] = direction_vector(210.0, -25.0) * 0.8
        elevations = [item.elevation_degrees for item in decode_multi_accdoa(
            signed, (0.3, 0.3), enabled_classes=(False, True)
        )]
        self.assertAlmostEqual(elevations[0], 35.0, places=4)
        self.assertAlmostEqual(elevations[1], -25.0, places=4)

    def test_perfect_outputs_pass_locked_synthetic_metrics(self):
        rows = []
        outputs = []
        combinations = (
            (),
            (("footstep", 0.0, 0.0),),
            (("footstep", 0.0, 0.0), ("gunshot", 25.0, 0.0)),
            (("footstep", 0.0, 0.0), ("footstep", 20.0, 5.0),
             ("gunshot", 180.0, -10.0)),
        )
        for index, combination in enumerate(combinations):
            targets = []
            tensor = np.zeros(DIRECTION_OUTPUT_SHAPE, dtype=np.float32)
            tracks = {"gunshot": 0, "footstep": 0}
            for name, azimuth, elevation in combination:
                targets.append({
                    "sound_class": name,
                    "rendered_direction": {
                        "azimuth_degrees": azimuth,
                        "elevation_degrees": elevation,
                    },
                })
                class_index = ("gunshot", "footstep").index(name)
                tensor[class_index, tracks[name]] = direction_vector(azimuth, elevation)
                tracks[name] += 1
            rows.append({
                "target_count": len(combination),
                "stratum": "close-15-30" if index >= 2 else "standard",
                "targets": targets,
            })
            outputs.append(tensor)
        metrics = evaluate_direction_outputs(rows, np.stack(outputs), (0.5, 0.5))
        self.assertTrue(metrics.acceptance_passed)
        self.assertEqual(metrics.exact_count_accuracy, 1.0)
        self.assertLess(metrics.p90_matched_error_degrees, 1.0e-4)

    @unittest.skipUnless(TORCH_AVAILABLE, "PyTorch is installed by the locked training environment")
    def test_classwise_adpit_is_permutation_invariant(self):
        import torch

        target = torch.zeros((1, *DIRECTION_OUTPUT_SHAPE))
        target[0, 1, 0] = torch.tensor([0.0, 0.0, 1.0])
        target[0, 1, 1] = torch.tensor([1.0, 0.0, 0.0])
        prediction = torch.zeros_like(target)
        prediction[0, 1, 0] = target[0, 1, 1]
        prediction[0, 1, 1] = target[0, 1, 0]
        prediction[0, 1, 2] = target[0, 1, 1]
        self.assertAlmostEqual(float(classwise_adpit_mse(prediction, target)), 0.0, places=7)

    @unittest.skipUnless(TORCH_AVAILABLE, "PyTorch is installed by the locked training environment")
    def test_direction_model_shape_and_cpu_parameter_budget(self):
        import torch

        model = build_direction_model().eval()
        values = torch.zeros((2, 5, 48, 64), dtype=torch.float32)
        values[:, 1] = -100.0
        with torch.no_grad():
            output = model(values)
        self.assertEqual(tuple(output.shape), (2, 2, 3, 3))
        self.assertLessEqual(sum(parameter.numel() for parameter in model.parameters()), 2_000_000)


if __name__ == "__main__":
    unittest.main()
