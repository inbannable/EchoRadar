import unittest

import numpy as np

from echoradar_ml import CLASS_NAMES, CONTEXT_FRAMES, FFT_SIZE, HOP_SIZE, INPUT_CHANNELS, MEL_BINS
from echoradar_ml.training import ONSET_WEIGHTS, _targets, _window_ends, padded_window, peak_candidates


class OnsetTest(unittest.TestCase):
    def test_targets_are_short_causal_pulses_and_allow_overlap(self):
        onset = FFT_SIZE + 5 * HOP_SIZE
        events = [
            {"class": "gunshot", "onset_sample": onset, "end_sample": onset + 99999},
            {"class": "footstep", "onset_sample": onset, "end_sample": onset + 99999},
        ]
        targets = _targets(20, events)
        start = 6
        np.testing.assert_allclose(targets[start:start + 3, 0], ONSET_WEIGHTS)
        np.testing.assert_allclose(targets[start:start + 3, 1], ONSET_WEIGHTS)
        self.assertEqual(targets.shape[1], len(CLASS_NAMES))
        self.assertEqual(int(np.count_nonzero(targets)), 6)

    def test_every_onset_and_startup_frame_can_be_a_window_endpoint(self):
        targets = np.zeros((180, len(CLASS_NAMES)), dtype=np.float32)
        targets[121:124, 0] = ONSET_WEIGHTS
        ends = _window_ends(targets, 24)
        self.assertIn(1, ends)
        self.assertTrue({122, 123, 124}.issubset(ends))

    def test_startup_window_uses_silent_left_padding(self):
        features = np.zeros((3, INPUT_CHANNELS, MEL_BINS), dtype=np.float32)
        features[:, 1] = -70.0
        window = padded_window(features, 2)
        self.assertEqual(window.shape, (INPUT_CHANNELS, CONTEXT_FRAMES, MEL_BINS))
        self.assertTrue(np.all(window[1, :-2] == -100.0))
        self.assertTrue(np.all(window[1, -2:] == -70.0))

    def test_peak_picker_separates_burst_without_probability_rearm(self):
        samples = np.arange(12, dtype=np.int64) * (10 * 48_000 // 1000)
        probabilities = np.asarray(
            [0.1, 0.4, 0.9, 0.7, 0.65, 0.7, 0.8, 0.7, 0.95, 0.6, 0.2, 0.1],
            dtype=np.float32,
        )
        activity = np.zeros_like(probabilities)
        candidates = peak_candidates(samples, probabilities, activity, 0.6, 0.8, 0.5,
                                     35 * 48_000 // 1000, lookahead_frames=1)
        self.assertEqual([candidate[0] for candidate in candidates], [samples[2], samples[8]])


if __name__ == "__main__":
    unittest.main()
