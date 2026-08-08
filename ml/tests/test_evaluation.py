import unittest

from echoradar_ml.evaluation import _rapid_gunshots, _source_metrics, calculate_metrics, match_events
from echoradar_ml.inference import PredictedEvent


class EvaluationTest(unittest.TestCase):
    def test_matching_keeps_overlapping_classes_independent(self):
        truth = [
            {"class": "gunshot", "onset_sample": 1000, "end_sample": 3400},
            {"class": "footstep", "onset_sample": 1200, "end_sample": 3600},
        ]
        predictions = [
            PredictedEvent("gunshot", 1000, 3400, 0.9, 1500),
            PredictedEvent("footstep", 1200, 3600, 0.8, 1700),
        ]
        pairs, _, _ = match_events(truth, predictions)
        self.assertEqual(len(pairs), 2)
        metrics = calculate_metrics(truth, predictions, 1.0)
        self.assertEqual(metrics["gunshot"].recall, 1.0)
        self.assertEqual(metrics["footstep"].recall, 1.0)

    def test_rapid_gunshots_require_two_matched_onsets(self):
        truth = [
            {"class": "gunshot", "onset_sample": 1000},
            {"class": "gunshot", "onset_sample": 3880},
        ]
        predictions = [PredictedEvent("gunshot", event["onset_sample"],
                                      event["onset_sample"] + 2400, 0.9)
                       for event in truth]
        self.assertEqual(_rapid_gunshots(truth, predictions)["rate"], 1.0)

    def test_source_metrics_measure_suppression_separately_from_detection(self):
        truth = [
            {"class": "gunshot", "onset_sample": 1000, "source_hint": "self"},
            {"class": "gunshot", "onset_sample": 5000, "source_hint": "remote"},
        ]
        raw = [
            PredictedEvent("gunshot", 1000, 3400, 0.9, suppressed=True),
            PredictedEvent("gunshot", 5000, 7400, 0.9),
        ]
        emitted = raw[1:]
        metrics = _source_metrics(truth, raw, emitted)
        self.assertEqual(metrics["self_suppression_rate"], 1.0)
        self.assertEqual(metrics["remote_recall_loss"], 0.0)


if __name__ == "__main__":
    unittest.main()
