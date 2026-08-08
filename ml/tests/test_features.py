import unittest

import numpy as np

from echoradar_ml import FFT_SIZE, MEL_BINS, SAMPLE_RATE
from echoradar_ml.features import (
    log_mel, scene_activity, stereo_mel_energy, stereo_onset_features, stereo_pcen,
    stereo_pcen_from_energy,
)


class FeaturesTest(unittest.TestCase):
    def test_shape_and_stereo_downmix(self):
        times = np.arange(FFT_SIZE * 2, dtype=np.float32) / SAMPLE_RATE
        tone = np.sin(2 * np.pi * 1000 * times).astype(np.float32)
        stereo = np.column_stack((tone, tone))
        features = log_mel(stereo)
        self.assertEqual(features.shape[1], MEL_BINS)
        self.assertGreater(float(features.max()), 0.0)

    def test_opposite_phase_cancels_in_mono(self):
        tone = np.ones(FFT_SIZE, dtype=np.float32)
        features = log_mel(np.column_stack((tone, -tone)))
        self.assertTrue(np.allclose(features, 0.0))

    def test_stereo_pcen_preserves_opposite_phase_energy_and_channel_swap(self):
        times = np.arange(FFT_SIZE * 3, dtype=np.float32) / SAMPLE_RATE
        tone = np.sin(2 * np.pi * 1400 * times).astype(np.float32)
        opposite = np.column_stack((tone, -tone))
        features = stereo_pcen(opposite)
        swapped = stereo_pcen(opposite[:, ::-1])
        self.assertEqual(features.shape[1:], (2, MEL_BINS))
        self.assertGreater(float(features[:, 0].max()), 0.0)
        np.testing.assert_allclose(features, swapped, atol=1e-6)

    def test_stereo_pcen_is_more_gain_stable_than_absolute_logmel(self):
        rng = np.random.default_rng(42)
        samples = rng.normal(0.0, 0.05, (FFT_SIZE * 8, 2)).astype(np.float32)
        loud = stereo_pcen(samples)
        quiet = stereo_pcen(samples * np.float32(10.0 ** (-24.0 / 20.0)))
        pcen_delta = float(np.mean(np.abs(loud[:, 0] - quiet[:, 0])))
        absolute_delta = float(np.mean(np.abs(loud[:, 1] - quiet[:, 1])))
        self.assertLess(pcen_delta, absolute_delta)

    def test_cached_energy_gain_matches_direct_audio_gain(self):
        rng = np.random.default_rng(7)
        samples = rng.normal(0.0, 0.04, (FFT_SIZE * 5, 2)).astype(np.float32)
        expected = stereo_pcen(samples * np.float32(10.0 ** (-12.0 / 20.0)))
        actual = stereo_pcen_from_energy(stereo_mel_energy(samples), -12.0)
        np.testing.assert_allclose(actual, expected, atol=2e-5, rtol=2e-5)

    def test_v4_spatial_planes_transform_predictably_on_channel_swap(self):
        times = np.arange(FFT_SIZE * 3, dtype=np.float32) / SAMPLE_RATE
        left = np.sin(2 * np.pi * 900 * times).astype(np.float32)
        right = 0.5 * np.roll(left, 3)
        features = stereo_onset_features(np.column_stack((left, right)))
        swapped = stereo_onset_features(np.column_stack((right, left)))
        self.assertEqual(features.shape[1:], (5, MEL_BINS))
        np.testing.assert_allclose(features[:, :2], swapped[:, :2], atol=2e-5, rtol=2e-5)
        np.testing.assert_allclose(features[:, 2], -swapped[:, 2], atol=2e-5, rtol=2e-5)
        np.testing.assert_allclose(features[:, 3], swapped[:, 3], atol=2e-5, rtol=2e-5)
        np.testing.assert_allclose(features[:, 4], -swapped[:, 4], atol=2e-5, rtol=2e-5)
        activity = scene_activity(features)
        self.assertEqual(activity.shape, (len(features),))
        self.assertTrue(np.all((activity >= 0.0) & (activity <= 1.0)))


if __name__ == "__main__":
    unittest.main()
