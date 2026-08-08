import unittest

import torch

from echoradar_ml.model import build_model


class ModelTest(unittest.TestCase):
    def test_two_heads_accept_stereo_pcen_contract(self):
        model = build_model(2).eval()
        with torch.no_grad():
            objectness, classes = model(torch.zeros((1, 2, 96, 64)))
        self.assertEqual(tuple(objectness.shape), (1, 96, 1))
        self.assertEqual(tuple(classes.shape), (1, 96, 3))
        probabilities = torch.sigmoid(objectness) * torch.sigmoid(classes)
        self.assertTrue(bool(torch.all((0.0 <= probabilities) & (probabilities <= 1.0))))


if __name__ == "__main__":
    unittest.main()
