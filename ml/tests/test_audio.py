from pathlib import Path
import tempfile
import unittest

import numpy as np

from echoradar_ml.audio import (
    Audio, load_pcm_wav, to_stereo_48k, trim_activity_with_offset, write_pcm16_wav,
)


class AudioTest(unittest.TestCase):
    def test_pcm16_round_trip_and_resample(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tone.wav"
            times = np.arange(4410, dtype=np.float32) / 44100.0
            samples = (0.5 * np.sin(2 * np.pi * 440 * times))[:, None]
            write_pcm16_wav(path, Audio(samples, 44100))
            converted = to_stereo_48k(load_pcm_wav(path))
            self.assertEqual(converted.samples.shape, (4800, 2))
            self.assertTrue(np.allclose(converted.samples[:, 0], converted.samples[:, 1]))
            self.assertLess(abs(float(np.max(converted.samples)) - 0.5), 0.01)

    def test_activity_trim_reports_audible_offset_not_preroll(self):
        samples = np.zeros((5000, 2), dtype=np.float32)
        samples[1500:2500] = 0.2
        trimmed, onset_offset = trim_activity_with_offset(Audio(samples, 48000))
        self.assertEqual(onset_offset, 960)
        self.assertAlmostEqual(float(trimmed.samples[onset_offset, 0]), 0.2, places=6)
        self.assertEqual(float(trimmed.samples[onset_offset - 1, 0]), 0.0)


if __name__ == "__main__":
    unittest.main()
