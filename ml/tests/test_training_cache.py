from pathlib import Path
import tempfile
import unittest

import numpy as np

from echoradar_ml.audio import Audio, write_pcm16_wav
from echoradar_ml.training import CachedWindowDataset, prepare_feature_cache


class TrainingCacheTest(unittest.TestCase):
    def test_disk_backed_windows_include_positive_and_negative_examples(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "train_real_cache"
            samples = np.zeros((48_000 * 2, 2), dtype=np.float32)
            samples[24_000:24_400] = np.hanning(400)[:, None] * 0.2
            write_pcm16_wav(prefix.with_suffix(".wav"), Audio(samples, 48_000))
            prefix.with_suffix(".jsonl").write_text(
                '{"class":"gunshot","onset_sample":24000,"end_sample":26400,'
                '"source_hint":"remote","reviewed":true}\n', encoding="utf-8"
            )
            records = prepare_feature_cache([prefix], root / "cache", (0.0,), stride_frames=20)
            dataset = CachedWindowDataset(records)
            self.assertGreater(len(dataset.positive_indices), 0)
            self.assertGreater(len(dataset.negative_indices), 0)
            values, onset, source = dataset[dataset.positive_indices[0]]
            self.assertEqual(values.shape, (5, 128, 64))
            self.assertEqual(onset.shape, (2,))
            self.assertEqual(source.shape, (2,))


if __name__ == "__main__":
    unittest.main()
