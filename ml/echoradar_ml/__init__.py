"""EchoRadar's deterministic current recognition and direction pipeline."""

SAMPLE_RATE = 48_000
FFT_SIZE = 1_024
HOP_SIZE = 240
MEL_BINS = 64
CONTEXT_FRAMES = 128
CLASS_NAMES = ("gunshot", "footstep")
SOURCE_NAMES = ("self", "remote", "unknown")
PREPROCESSING_VERSION = "stereo-onset-v4"
INPUT_CHANNELS = 5
INFERENCE_STRIDE_FRAMES = 2
PCEN_SMOOTHING = 0.025
PCEN_ALPHA = 0.98
PCEN_DELTA = 2.0
PCEN_ROOT = 0.5
PCEN_EPSILON = 1e-6
