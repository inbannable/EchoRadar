"""EchoRadar's deterministic offline sound-recognition pipeline."""

SAMPLE_RATE = 48_000
FFT_SIZE = 1_024
HOP_SIZE = 512
MEL_BINS = 64
CONTEXT_FRAMES = 96
CLASS_NAMES = ("gunshot", "footstep", "mechanical")
PREPROCESSING_VERSION = "logmel-v1"
ONSET_PREPROCESSING_VERSION = "stereo-pcen-v2"
INPUT_CHANNELS = 2
PCEN_SMOOTHING = 0.025
PCEN_ALPHA = 0.98
PCEN_DELTA = 2.0
PCEN_ROOT = 0.5
PCEN_EPSILON = 1e-6
