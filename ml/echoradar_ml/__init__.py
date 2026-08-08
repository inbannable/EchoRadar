"""EchoRadar's deterministic offline sound-recognition pipeline.

The unqualified constants describe the training/deployment v4 contract.  The
legacy constants remain explicit because old model packages are still useful
for regression comparisons, but they must never silently select v4 shapes.
"""

SAMPLE_RATE = 48_000
FFT_SIZE = 1_024
HOP_SIZE = 240
MEL_BINS = 64
CONTEXT_FRAMES = 128
CLASS_NAMES = ("gunshot", "footstep")
SOURCE_NAMES = ("self", "remote", "unknown")
PREPROCESSING_VERSION = "logmel-v1"
ONSET_PREPROCESSING_VERSION = "stereo-pcen-v2"
V4_PREPROCESSING_VERSION = "stereo-onset-v4"
INPUT_CHANNELS = 5
INFERENCE_STRIDE_FRAMES = 2

LEGACY_HOP_SIZE = 512
LEGACY_CONTEXT_FRAMES = 96
LEGACY_CLASS_NAMES = ("gunshot", "footstep", "mechanical")
PCEN_SMOOTHING = 0.025
PCEN_ALPHA = 0.98
PCEN_DELTA = 2.0
PCEN_ROOT = 0.5
PCEN_EPSILON = 1e-6
