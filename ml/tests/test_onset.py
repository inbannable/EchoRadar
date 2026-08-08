import unittest

import numpy as np

from echoradar_ml import CLASS_NAMES, FFT_SIZE, HOP_SIZE
from echoradar_ml.inference import postprocess
from echoradar_ml.training import _targets, _window_ends


class OnsetTest(unittest.TestCase):
    def test_targets_are_short_causal_pulses_and_allow_overlap(self):
        onset = FFT_SIZE + 5 * HOP_SIZE
        events = [
            {"class": "gunshot", "onset_sample": onset, "end_sample": onset + 99999},
            {"class": "footstep", "onset_sample": onset, "end_sample": onset + 99999},
        ]
        targets = _targets(20, events)
        start = 6
        np.testing.assert_allclose(targets[start:start + 3, 0], [1.0, 0.5, 0.25])
        np.testing.assert_allclose(targets[start:start + 3, 1], [1.0, 0.5, 0.25])
        self.assertEqual(int(np.count_nonzero(targets[:, 2])), 0)
        self.assertEqual(int(np.count_nonzero(targets[:, :2])), 6)

    def test_every_onset_frame_is_used_as_a_window_endpoint(self):
        targets = np.zeros((180, 3), dtype=np.float32)
        targets[121:124, 0] = (1.0, 0.5, 0.25)
        ends = _window_ends(targets, 24)
        self.assertTrue({122, 123, 124}.issubset(ends))

    def test_onset_postprocessor_emits_immediate_fixed_pulses_and_rearms(self):
        metadata = {
            "event_mode": "onset-pulse", "pulse_ms": 50,
        }
        for index, name in enumerate(CLASS_NAMES):
            metadata[f"threshold_{name}"] = 0.6
            metadata[f"rearm_threshold_{name}"] = 0.3
            metadata[f"refractory_ms_{name}"] = (40, 60, 80)[index]
            metadata[f"onset_offset_samples_{name}"] = 100
        samples = np.asarray([1000, 2024, 3048], dtype=np.int64)
        probabilities = np.asarray([
            [0.9, 0.8, 0.1],
            [0.1, 0.1, 0.1],
            [0.9, 0.1, 0.1],
        ], dtype=np.float32)
        events = postprocess(samples, probabilities, metadata)
        self.assertEqual([event.sound_class for event in events], ["gunshot", "footstep", "gunshot"])
        self.assertTrue(all(event.end_sample - event.onset_sample == 2400 for event in events))
        self.assertEqual(events[0].detected_sample, 1000)


if __name__ == "__main__":
    unittest.main()
