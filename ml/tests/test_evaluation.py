import unittest

from echoradar_ml.evaluation import _overlap_case_rate, _rapid_separation
from echoradar_ml.inference import PredictedEvent


class EvaluationTest(unittest.TestCase):
    def test_overlap_requires_both_independent_classes(self):
        truth = [
            {"class": "gunshot", "onset_sample": 1000, "overlap": True},
            {"class": "footstep", "onset_sample": 1200, "overlap": True},
        ]
        complete = [
            PredictedEvent("gunshot", 1000, 3400, 0.9),
            PredictedEvent("footstep", 1200, 3600, 0.8),
        ]
        self.assertEqual(_overlap_case_rate(truth, complete)["rate"], 1.0)
        self.assertEqual(_overlap_case_rate(truth, complete[:1])["rate"], 0.0)

    def test_rapid_separation_requires_two_matched_onsets(self):
        truth = [
            {"class": "gunshot", "onset_sample": 1000},
            {"class": "gunshot", "onset_sample": 3880},
            {"class": "footstep", "onset_sample": 10000},
            {"class": "footstep", "onset_sample": 14800},
        ]
        predictions = [PredictedEvent(event["class"], event["onset_sample"],
                                      event["onset_sample"] + 2400, 0.9)
                       for event in truth]
        result = _rapid_separation(truth, predictions)
        self.assertEqual(result["gunshot"]["rate"], 1.0)
        self.assertEqual(result["footstep"]["rate"], 1.0)


if __name__ == "__main__":
    unittest.main()
