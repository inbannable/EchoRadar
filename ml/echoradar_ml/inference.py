from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path

import numpy as np

from . import (
    CLASS_NAMES, CONTEXT_FRAMES, FFT_SIZE, HOP_SIZE, MEL_BINS, ONSET_PREPROCESSING_VERSION,
    PCEN_ALPHA, PCEN_DELTA, PCEN_EPSILON, PCEN_ROOT, PCEN_SMOOTHING,
    PREPROCESSING_VERSION, SAMPLE_RATE,
)
from .audio import Audio, load_pcm_wav, to_stereo_48k, write_pcm16_wav
from .features import recognition_features


def require_onnxruntime():
    try:
        import onnxruntime
    except ImportError as error:
        raise RuntimeError("inference requires: pip install -e 'ml[train]'") from error
    return onnxruntime


@dataclass(frozen=True)
class PredictedEvent:
    sound_class: str
    onset_sample: int
    end_sample: int
    confidence: float
    detected_sample: int = 0


def load_package(directory: str | Path) -> tuple[dict, Path]:
    root = Path(directory)
    metadata = json.loads((root / "model.json").read_text(encoding="utf-8"))
    preprocessing_version = metadata.get("preprocessing_version")
    if preprocessing_version not in (PREPROCESSING_VERSION, ONSET_PREPROCESSING_VERSION):
        raise ValueError(f"incompatible model metadata preprocessing_version: {preprocessing_version!r}")
    required = {
        "sample_rate": SAMPLE_RATE,
        "fft_size": FFT_SIZE, "hop_size": HOP_SIZE, "mel_bins": MEL_BINS,
        "context_frames": CONTEXT_FRAMES, "class_order": ",".join(CLASS_NAMES),
    }
    for key, expected in required.items():
        if metadata.get(key) != expected:
            raise ValueError(f"incompatible model metadata {key}: {metadata.get(key)!r}")
    expected_channels = 2 if preprocessing_version == ONSET_PREPROCESSING_VERSION else 1
    if int(metadata.get("input_channels", expected_channels)) != expected_channels:
        raise ValueError("incompatible model input_channels")
    if preprocessing_version == ONSET_PREPROCESSING_VERSION:
        if metadata.get("event_mode") != "onset-pulse" or int(metadata.get("pulse_ms", 0)) <= 0:
            raise ValueError("stereo-pcen-v2 requires onset-pulse metadata")
        pcen_required = {
            "pcen_smoothing": PCEN_SMOOTHING, "pcen_alpha": PCEN_ALPHA,
            "pcen_delta": PCEN_DELTA, "pcen_root": PCEN_ROOT,
            "pcen_epsilon": PCEN_EPSILON,
        }
        for key, expected in pcen_required.items():
            if not np.isclose(float(metadata.get(key, float("nan"))), expected):
                raise ValueError(f"incompatible model metadata {key}: {metadata.get(key)!r}")
    if not isinstance(metadata.get("inference_stride_frames"), int) or metadata["inference_stride_frames"] <= 0:
        raise ValueError("incompatible model inference stride")
    for name in CLASS_NAMES:
        required_keys = (f"threshold_{name}", f"off_threshold_{name}",
                         f"min_on_frames_{name}", f"min_off_frames_{name}",
                         f"refractory_ms_{name}")
        if any(key not in metadata for key in required_keys):
            raise ValueError(f"model metadata is missing post-processing values for {name}")
        on = float(metadata[f"threshold_{name}"])
        off = float(metadata[f"off_threshold_{name}"])
        if not (0.0 < on <= 1.0 and 0.0 <= off < on and
                int(metadata[f"min_on_frames_{name}"]) > 0 and
                int(metadata[f"min_off_frames_{name}"]) > 0 and
                int(metadata[f"refractory_ms_{name}"]) >= 0):
            raise ValueError(f"invalid post-processing values for {name}")
        if preprocessing_version == ONSET_PREPROCESSING_VERSION:
            for key in (f"rearm_threshold_{name}", f"onset_offset_samples_{name}"):
                if key not in metadata:
                    raise ValueError(f"model metadata is missing post-processing value {key}")
    model_name = Path(metadata["model_file"])
    if model_name.is_absolute() or len(model_name.parts) != 1:
        raise ValueError("model file must be inside the package directory")
    model_path = root / model_name
    digest = hashlib.sha256(model_path.read_bytes()).hexdigest()
    if digest != metadata["model_sha256"]:
        raise ValueError("model SHA-256 does not match model.json")
    return metadata, model_path


def probabilities_for_audio(audio: Audio, package_dir: str | Path) -> tuple[np.ndarray, np.ndarray, dict]:
    metadata, model_path = load_package(package_dir)
    ort = require_onnxruntime()
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    features = recognition_features(to_stereo_48k(audio).samples, metadata["preprocessing_version"])
    stride = int(metadata["inference_stride_frames"])
    samples: list[int] = []
    probabilities: list[np.ndarray] = []
    for end in range(CONTEXT_FRAMES, len(features) + 1, stride):
        window = features[end - CONTEXT_FRAMES:end]
        values = window.transpose(1, 0, 2)[None, :, :, :]
        output = session.run(["probabilities"], {"logmel": values})[0]
        probabilities.append(output[0, -1].astype(np.float32))
        samples.append((end - 1) * HOP_SIZE + FFT_SIZE)
    return np.asarray(samples, dtype=np.int64), np.asarray(probabilities, dtype=np.float32), metadata


def postprocess(samples: np.ndarray, probabilities: np.ndarray, metadata: dict) -> list[PredictedEvent]:
    if metadata.get("event_mode", "segment") == "onset-pulse":
        states = [dict(armed=True, refractory=0) for _ in CLASS_NAMES]
        events: list[PredictedEvent] = []
        pulse_samples = int(metadata["pulse_ms"]) * SAMPLE_RATE // 1000
        for sample, values in zip(samples, probabilities, strict=True):
            sample = int(sample)
            for index, name in enumerate(CLASS_NAMES):
                state = states[index]
                probability = float(np.clip(values[index], 0.0, 1.0))
                rearm = float(metadata[f"rearm_threshold_{name}"])
                if not state["armed"]:
                    if probability < rearm:
                        state["armed"] = True
                    continue
                if sample < state["refractory"] or probability < float(metadata[f"threshold_{name}"]):
                    continue
                onset = max(0, sample - int(metadata[f"onset_offset_samples_{name}"]))
                events.append(PredictedEvent(name, onset, onset + pulse_samples, probability, sample))
                state["armed"] = False
                state["refractory"] = sample + int(metadata[f"refractory_ms_{name}"]) * SAMPLE_RATE // 1000
        return events

    states = [dict(active=False, above=0, below=0, onset=0, candidate=0, peak=0.0, refractory=0)
              for _ in CLASS_NAMES]
    events: list[PredictedEvent] = []
    for sample, values in zip(samples, probabilities, strict=True):
        sample = int(sample)
        for index, name in enumerate(CLASS_NAMES):
            state = states[index]
            on = float(metadata[f"threshold_{name}"])
            off = float(metadata[f"off_threshold_{name}"])
            min_on = int(metadata[f"min_on_frames_{name}"])
            min_off = int(metadata[f"min_off_frames_{name}"])
            refractory = int(metadata[f"refractory_ms_{name}"]) * SAMPLE_RATE // 1000
            probability = float(np.clip(values[index], 0.0, 1.0))
            if not state["active"]:
                if sample < state["refractory"] or probability < on:
                    state["above"] = 0
                    continue
                if state["above"] == 0:
                    state["candidate"] = sample
                    state["peak"] = 0.0
                state["above"] += 1
                state["peak"] = max(state["peak"], probability)
                if state["above"] >= min_on:
                    state["active"] = True
                    state["onset"] = state["candidate"]
                    state["below"] = 0
                continue
            state["peak"] = max(state["peak"], probability)
            state["below"] = state["below"] + 1 if probability < off else 0
            if state["below"] < min_off:
                continue
            events.append(PredictedEvent(name, state["onset"], sample, state["peak"]))
            states[index] = dict(active=False, above=0, below=0, onset=0, candidate=0,
                                 peak=0.0, refractory=sample + refractory)
    end_sample = int(samples[-1]) if len(samples) else 0
    for index, name in enumerate(CLASS_NAMES):
        state = states[index]
        if state["active"]:
            events.append(PredictedEvent(name, state["onset"], end_sample, state["peak"]))
    return events


def predict_wav(path: str | Path, package_dir: str | Path) -> tuple[list[PredictedEvent], np.ndarray, np.ndarray]:
    audio = load_pcm_wav(path)
    samples, probabilities, metadata = probabilities_for_audio(audio, package_dir)
    return postprocess(samples, probabilities, metadata), samples, probabilities


def check_onnx_parity(package_dir: str | Path, fixture: str | Path, tolerance: float = 1e-4) -> float:
    _, model_path = load_package(package_dir)
    fixture_data = np.load(fixture)
    ort = require_onnxruntime()
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    actual = session.run(["probabilities"], {"logmel": fixture_data["input"].astype(np.float32)})[0]
    maximum_error = float(np.max(np.abs(actual - fixture_data["torch_output"])))
    if maximum_error > tolerance:
        raise ValueError(f"PyTorch/ONNX parity failed: max error {maximum_error} > {tolerance}")
    return maximum_error
