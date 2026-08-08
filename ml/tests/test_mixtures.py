import tempfile
from pathlib import Path
import unittest

import numpy as np

from echoradar_ml.audio import Audio, write_pcm16_wav
from echoradar_ml.manifest import Asset
from echoradar_ml.mixtures import generate_session


class MixtureTest(unittest.TestCase):
    def test_sessions_are_deterministic_multilabel_and_complex_overlap_is_explicit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            assets = []
            for index, label in enumerate(("gunshot", "footstep", "mechanical", "other")):
                relative = f"{label}.wav"
                samples = np.zeros((4800, 1), dtype=np.float32)
                samples[200:1200, 0] = 0.1 * np.sin(
                    np.linspace(0.0, np.pi * (index + 2) * 20, 1000, dtype=np.float32)
                )
                write_pcm16_wav(root / relative, Audio(samples, 48000))
                assets.append(Asset(label, relative, label, f"group-{label}", label, "", True, "test"))

            first_audio, first_timeline = generate_session(assets, root, "test", "complex", 8.0, 17)
            second_audio, second_timeline = generate_session(assets, root, "test", "complex", 8.0, 17)

            np.testing.assert_array_equal(first_audio.samples, second_audio.samples)
            self.assertEqual(first_timeline, second_timeline)
            self.assertEqual({event.sound_class for event in first_timeline},
                             {"gunshot", "footstep", "negative"})
            self.assertTrue(any(event.overlap for event in first_timeline))
            self.assertTrue(all(not event.seen_source for event in first_timeline))
            self.assertTrue(all(event.end_sample > event.onset_sample for event in first_timeline))
            self.assertTrue(all(event.source_hint in ("self", "remote", "unknown")
                                for event in first_timeline))
            by_class = {
                name: sorted(event.onset_sample for event in first_timeline if event.sound_class == name)
                for name in ("gunshot", "footstep")
            }
            self.assertIn(60 * 48000 // 1000,
                          [second - first for first, second in zip(by_class["gunshot"], by_class["gunshot"][1:])])
            self.assertIn(100 * 48000 // 1000,
                          [second - first for first, second in zip(by_class["footstep"], by_class["footstep"][1:])])


if __name__ == "__main__":
    unittest.main()
