from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path

import numpy as np

from . import (
    CLASS_NAMES, CONTEXT_FRAMES, FFT_SIZE, HOP_SIZE, INPUT_CHANNELS,
    LEGACY_CLASS_NAMES, LEGACY_CONTEXT_FRAMES, LEGACY_HOP_SIZE, MEL_BINS,
    ONSET_PREPROCESSING_VERSION, PCEN_ALPHA, PCEN_DELTA, PCEN_EPSILON,
    PCEN_ROOT, PCEN_SMOOTHING, PREPROCESSING_VERSION, SAMPLE_RATE, SOURCE_NAMES,
    V4_PREPROCESSING_VERSION,
)
from .audio import Audio, load_pcm_wav, to_stereo_48k
from .features import recognition_features, scene_activity
from .training import padded_window, peak_candidates


def require_onnxruntime():
    try:
        import onnxruntime
    except ImportError as error:
        raise RuntimeError("inference requires: pip install -r ml/requirements-lock.txt") from error
    return onnxruntime


@dataclass(frozen=True)
class PredictedEvent:
    sound_class: str
    onset_sample: int
    end_sample: int
    confidence: float
    detected_sample: int = 0
    source_hint: str = "unknown"
    source_confidence: float = 0.0
    scene_mode: str = "unknown"
    suppressed: bool = False


@dataclass(frozen=True)
class ProbabilityTrace:
    samples: np.ndarray
    probabilities: np.ndarray
    source_probabilities: np.ndarray
    activity: np.ndarray
    metadata: dict


def load_package(directory: str | Path) -> tuple[dict, Path]:
    root = Path(directory)
    metadata = json.loads((root / "model.json").read_text(encoding="utf-8"))
    version = metadata.get("preprocessing_version")
    if version == V4_PREPROCESSING_VERSION:
        required = {
            "sample_rate": SAMPLE_RATE, "fft_size": FFT_SIZE, "hop_size": HOP_SIZE,
            "mel_bins": MEL_BINS, "context_frames": CONTEXT_FRAMES,
            "input_channels": INPUT_CHANNELS, "class_order": ",".join(CLASS_NAMES),
            "source_order": ",".join(SOURCE_NAMES), "event_mode": "onset-peak",
        }
        for key, expected in required.items():
            if metadata.get(key) != expected:
                raise ValueError(f"incompatible v4 model metadata {key}: {metadata.get(key)!r}")
        scalar_keys = ["scene_activity_cutoff", "self_suppression_threshold", "peak_lookahead_frames"]
        for name in CLASS_NAMES:
            scalar_keys.extend((
                f"threshold_quiet_{name}", f"threshold_busy_{name}",
                f"minimum_spacing_ms_{name}", f"onset_offset_samples_{name}",
            ))
        if any(key not in metadata for key in scalar_keys):
            raise ValueError("v4 model metadata is missing calibrated policy values")
        if not (0.0 < float(metadata["scene_activity_cutoff"]) < 1.0
                and 0.0 < float(metadata["self_suppression_threshold"]) <= 1.0
                and int(metadata["peak_lookahead_frames"]) >= 0
                and int(metadata.get("pulse_ms", 0)) > 0):
            raise ValueError("v4 model scene/source/peak policy is invalid")
        for name in CLASS_NAMES:
            quiet = float(metadata[f"threshold_quiet_{name}"])
            busy = float(metadata[f"threshold_busy_{name}"])
            if not (0.0 < quiet <= 1.0 and 0.0 < busy <= 1.0
                    and int(metadata[f"minimum_spacing_ms_{name}"]) > 0
                    and int(metadata[f"onset_offset_samples_{name}"]) >= 0):
                raise ValueError(f"v4 calibrated policy is invalid for {name}")
    elif version in (PREPROCESSING_VERSION, ONSET_PREPROCESSING_VERSION):
        required = {
            "sample_rate": SAMPLE_RATE, "fft_size": FFT_SIZE, "hop_size": LEGACY_HOP_SIZE,
            "mel_bins": MEL_BINS, "context_frames": LEGACY_CONTEXT_FRAMES,
            "class_order": ",".join(LEGACY_CLASS_NAMES),
        }
        for key, expected in required.items():
            if metadata.get(key) != expected:
                raise ValueError(f"incompatible legacy model metadata {key}: {metadata.get(key)!r}")
        expected_channels = 2 if version == ONSET_PREPROCESSING_VERSION else 1
        if int(metadata.get("input_channels", expected_channels)) != expected_channels:
            raise ValueError("incompatible legacy model input_channels")
    else:
        raise ValueError(f"incompatible model preprocessing_version: {version!r}")

    if version in (ONSET_PREPROCESSING_VERSION, V4_PREPROCESSING_VERSION):
        for key, expected in {
            "pcen_smoothing": PCEN_SMOOTHING, "pcen_alpha": PCEN_ALPHA,
            "pcen_delta": PCEN_DELTA, "pcen_root": PCEN_ROOT,
            "pcen_epsilon": PCEN_EPSILON,
        }.items():
            if not np.isclose(float(metadata.get(key, float("nan"))), expected):
                raise ValueError(f"incompatible model metadata {key}: {metadata.get(key)!r}")
    if not isinstance(metadata.get("inference_stride_frames"), int) or metadata["inference_stride_frames"] <= 0:
        raise ValueError("incompatible model inference stride")

    model_name = Path(metadata["model_file"])
    if model_name.is_absolute() or len(model_name.parts) != 1:
        raise ValueError("model file must be inside the package directory")
    model_path = root / model_name
    digest = hashlib.sha256(model_path.read_bytes()).hexdigest()
    if digest != metadata["model_sha256"]:
        raise ValueError("model SHA-256 does not match model.json")
    return metadata, model_path


def probabilities_for_audio(audio: Audio, package_dir: str | Path) -> ProbabilityTrace:
    metadata, model_path = load_package(package_dir)
    ort = require_onnxruntime()
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    version = metadata["preprocessing_version"]
    features = recognition_features(to_stereo_48k(audio).samples, version)
    stride = int(metadata["inference_stride_frames"])
    context = int(metadata["context_frames"])
    hop = int(metadata["hop_size"])
    if version == V4_PREPROCESSING_VERSION:
        ends = list(range(1, len(features) + 1, stride))
        if ends and ends[-1] != len(features):
            ends.append(len(features))
        activities_full = scene_activity(features, round(0.5 * SAMPLE_RATE / HOP_SIZE))
    else:
        ends = list(range(context, len(features) + 1, stride))
        activities_full = np.zeros(len(features), dtype=np.float32)

    samples: list[int] = []
    probabilities: list[np.ndarray] = []
    sources: list[np.ndarray] = []
    activities: list[float] = []
    for end in ends:
        if version == V4_PREPROCESSING_VERSION:
            values = padded_window(features, end)[None]
            onset_output, source_output = session.run(
                ["onset_probabilities", "source_probabilities"], {"features": values}
            )
            probabilities.append(onset_output[0, -1].astype(np.float32))
            sources.append(source_output[0, -1].astype(np.float32))
        else:
            window = features[end - context:end]
            values = window.transpose(1, 0, 2)[None]
            output = session.run(["probabilities"], {"logmel": values})[0]
            probabilities.append(output[0, -1].astype(np.float32))
            sources.append(np.zeros((len(LEGACY_CLASS_NAMES), len(SOURCE_NAMES)), dtype=np.float32))
        samples.append((end - 1) * hop + FFT_SIZE)
        activities.append(float(activities_full[end - 1]))
    class_count = len(CLASS_NAMES) if version == V4_PREPROCESSING_VERSION else len(LEGACY_CLASS_NAMES)
    return ProbabilityTrace(
        samples=np.asarray(samples, dtype=np.int64),
        probabilities=np.asarray(probabilities, dtype=np.float32).reshape(-1, class_count),
        source_probabilities=np.asarray(sources, dtype=np.float32).reshape(
            -1, class_count, len(SOURCE_NAMES)
        ),
        activity=np.asarray(activities, dtype=np.float32), metadata=metadata,
    )


def _postprocess_v4(trace: ProbabilityTrace, include_suppressed: bool) -> list[PredictedEvent]:
    metadata = trace.metadata
    cutoff = float(metadata["scene_activity_cutoff"])
    self_threshold = float(metadata["self_suppression_threshold"])
    lookahead = int(metadata["peak_lookahead_frames"])
    events: list[PredictedEvent] = []
    for class_index, name in enumerate(CLASS_NAMES):
        candidates = peak_candidates(
            trace.samples, trace.probabilities[:, class_index], trace.activity,
            float(metadata[f"threshold_quiet_{name}"]),
            float(metadata[f"threshold_busy_{name}"]), cutoff,
            int(metadata[f"minimum_spacing_ms_{name}"]) * SAMPLE_RATE // 1000,
            lookahead,
        )
        for sample, confidence, row in candidates:
            source_values = trace.source_probabilities[row, class_index]
            source_index = int(np.argmax(source_values))
            source_hint = SOURCE_NAMES[source_index]
            source_confidence = float(source_values[source_index])
            suppressed = float(source_values[SOURCE_NAMES.index("self")]) >= self_threshold
            if suppressed:
                source_hint = "self"
                source_confidence = float(source_values[SOURCE_NAMES.index("self")])
            onset = max(0, sample - int(metadata[f"onset_offset_samples_{name}"]))
            delivery_index = min(len(trace.samples) - 1, row + lookahead)
            event = PredictedEvent(
                name, onset, onset + int(metadata.get("pulse_ms", 50)) * SAMPLE_RATE // 1000,
                confidence, int(trace.samples[delivery_index]), source_hint, source_confidence,
                "quiet" if float(trace.activity[row]) < cutoff else "busy", suppressed,
            )
            if include_suppressed or not suppressed:
                events.append(event)
    return sorted(events, key=lambda event: (event.detected_sample, event.sound_class))


def _postprocess_legacy(trace: ProbabilityTrace) -> list[PredictedEvent]:
    metadata = trace.metadata
    names = LEGACY_CLASS_NAMES
    if metadata.get("event_mode", "segment") == "onset-pulse":
        states = [dict(armed=True, refractory=0) for _ in names]
        events: list[PredictedEvent] = []
        pulse_samples = int(metadata["pulse_ms"]) * SAMPLE_RATE // 1000
        for sample, values in zip(trace.samples, trace.probabilities, strict=True):
            for index, name in enumerate(names):
                state = states[index]
                probability = float(np.clip(values[index], 0.0, 1.0))
                if not state["armed"]:
                    if probability < float(metadata[f"rearm_threshold_{name}"]):
                        state["armed"] = True
                    continue
                if int(sample) < state["refractory"] or probability < float(metadata[f"threshold_{name}"]):
                    continue
                onset = max(0, int(sample) - int(metadata[f"onset_offset_samples_{name}"]))
                events.append(PredictedEvent(name, onset, onset + pulse_samples, probability, int(sample)))
                state["armed"] = False
                state["refractory"] = int(sample) + int(metadata[f"refractory_ms_{name}"]) * SAMPLE_RATE // 1000
        return events
    states = [dict(active=False, above=0, below=0, onset=0, candidate=0,
                   peak=0.0, refractory=0) for _ in names]
    events: list[PredictedEvent] = []
    for sample, values in zip(trace.samples, trace.probabilities, strict=True):
        sample = int(sample)
        for index, name in enumerate(names):
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
            events.append(PredictedEvent(
                name, state["onset"], sample, state["peak"], sample,
            ))
            states[index] = dict(active=False, above=0, below=0, onset=0, candidate=0,
                                 peak=0.0, refractory=sample + refractory)
    end_sample = int(trace.samples[-1]) if len(trace.samples) else 0
    for index, name in enumerate(names):
        state = states[index]
        if state["active"]:
            events.append(PredictedEvent(
                name, state["onset"], end_sample, state["peak"], end_sample,
            ))
    return events


def postprocess(trace: ProbabilityTrace, include_suppressed: bool = False) -> list[PredictedEvent]:
    if trace.metadata["preprocessing_version"] == V4_PREPROCESSING_VERSION:
        return _postprocess_v4(trace, include_suppressed)
    return _postprocess_legacy(trace)


def predict_wav(path: str | Path, package_dir: str | Path,
                include_suppressed: bool = False) -> tuple[list[PredictedEvent], ProbabilityTrace]:
    trace = probabilities_for_audio(load_pcm_wav(path), package_dir)
    return postprocess(trace, include_suppressed), trace


def check_onnx_parity(package_dir: str | Path, fixture: str | Path,
                      tolerance: float = 1e-4) -> float:
    metadata, model_path = load_package(package_dir)
    fixture_data = np.load(fixture)
    ort = require_onnxruntime()
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    if metadata["preprocessing_version"] == V4_PREPROCESSING_VERSION:
        actual_onset, actual_source = session.run(
            ["onset_probabilities", "source_probabilities"],
            {"features": fixture_data["input"].astype(np.float32)},
        )
        maximum_error = max(
            float(np.max(np.abs(actual_onset - fixture_data["torch_onset"]))),
            float(np.max(np.abs(actual_source - fixture_data["torch_source"]))),
        )
        if maximum_error > tolerance:
            raise ValueError(f"PyTorch/ONNX parity failed: max error {maximum_error} > {tolerance}")
        return maximum_error
    actual = session.run(
        ["probabilities"], {"logmel": fixture_data["input"].astype(np.float32)}
    )[0]
    maximum_error = float(np.max(np.abs(actual - fixture_data["torch_output"])))
    if maximum_error > tolerance:
        raise ValueError(f"PyTorch/ONNX parity failed: max error {maximum_error} > {tolerance}")
    return maximum_error
