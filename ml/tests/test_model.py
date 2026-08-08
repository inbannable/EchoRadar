import importlib.util
import unittest

from echoradar_ml import CLASS_NAMES, CONTEXT_FRAMES, INPUT_CHANNELS, MEL_BINS, SOURCE_NAMES
from echoradar_ml.model import build_model


TORCH_AVAILABLE = importlib.util.find_spec("torch") is not None


@unittest.skipUnless(TORCH_AVAILABLE, "PyTorch is installed by the locked training environment")
class ModelTest(unittest.TestCase):
    def test_v4_onset_and_source_heads(self):
        import torch

        model = build_model().eval()
        values = torch.zeros((1, INPUT_CHANNELS, CONTEXT_FRAMES, MEL_BINS))
        values[:, 1] = -100.0
        with torch.no_grad():
            onset, source = model(values)
        self.assertEqual(tuple(onset.shape), (1, CONTEXT_FRAMES, len(CLASS_NAMES)))
        self.assertEqual(tuple(source.shape),
                         (1, CONTEXT_FRAMES, len(CLASS_NAMES), len(SOURCE_NAMES)))
        self.assertLess(sum(parameter.numel() for parameter in model.parameters()), 1_000_000)


if __name__ == "__main__":
    unittest.main()
