from pathlib import Path
import tempfile
import unittest

import numpy as np

from echoradar_ml.audio import Audio, write_pcm16_wav
from echoradar_ml.sessions import (
    audit_session_corpus, import_real_session, load_timeline, training_readiness,
    validate_timeline,
)


class SessionsTest(unittest.TestCase):
    def test_timeline_requires_valid_source_and_bounds(self):
        events = validate_timeline([
            {"class": "footstep", "onset_sample": 100, "end_sample": 200,
             "source_hint": "remote", "reviewed": True},
            {"class": "negative", "onset_sample": 300, "end_sample": 400,
             "source_hint": "unknown", "reviewed": True},
        ], 1000)
        self.assertEqual(events[0]["source_hint"], "remote")
        with self.assertRaises(ValueError):
            validate_timeline([{"class": "footstep", "onset_sample": -1}], 1000)

    def test_real_import_normalizes_audio_and_is_auditable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            wav = root / "capture.wav"
            labels = root / "labels.jsonl"
            write_pcm16_wav(wav, Audio(np.zeros((44100, 2), dtype=np.float32), 44100))
            labels.write_text(
                '{"class":"gunshot","onset_sample":1000,"end_sample":2000,'
                '"source_hint":"remote","reviewed":true}\n', encoding="utf-8"
            )
            prefix = import_real_session(
                wav, labels, root / "sessions", "train", "session1", "de_dust2",
                "2026-08-08", "48k-headphones", label_sample_rate=44100,
            )
            self.assertTrue(prefix.with_suffix(".wav").exists())
            imported = load_timeline(prefix.with_suffix(".jsonl"))
            self.assertEqual(imported[0]["onset_sample"], round(1000 * 48000 / 44100))
            report = audit_session_corpus(root / "sessions")
            self.assertEqual(report["sessions"], 1)
            self.assertEqual(report["support"]["train:gunshot:remote"], 1)
            self.assertFalse(all(training_readiness(report).values()))


if __name__ == "__main__":
    unittest.main()
