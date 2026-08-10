from pathlib import Path
import json

import numpy as np

from echoradar_ml import SAMPLE_RATE
from echoradar_ml.audio import Audio, write_pcm16_wav
from echoradar_ml.direction import (
    BAND_COUNT,
    DirectionCorpusRow,
    DirectionFeatures,
    FEATURE_SCHEMA_VERSION,
    circular_error_degrees,
    extract_direction_features,
    evaluate_direction_rows,
    generate_direction_corpus,
    select_peak_window,
)


def _stereo_fixture(frames: int = 4096) -> np.ndarray:
    time = np.arange(frames, dtype=np.float32) / SAMPLE_RATE
    rng = np.random.default_rng(12345)
    tone = (0.7 * rng.normal(0.0, 1.0, frames) +
            0.3 * np.sin(2.0 * np.pi * 1200.0 * time)).astype(np.float32)
    samples = np.zeros((frames, 2), dtype=np.float32)
    samples[:, 0] = tone
    samples[3:, 1] = tone[:-3] * 0.45
    return samples


def test_channel_swap_mirrors_pooled_cues() -> None:
    original = extract_direction_features(_stereo_fixture())
    mirrored = extract_direction_features(_stereo_fixture()[:, ::-1])
    assert np.isclose(original.broadband_ild_db, -mirrored.broadband_ild_db, atol=0.01)
    assert np.isclose(original.gcc_delay_samples, -mirrored.gcc_delay_samples, atol=0.01)
    assert original.schema_version == FEATURE_SCHEMA_VERSION
    assert len(original.band_ild_db) == BAND_COUNT
    assert np.allclose(original.left_spectral_shape, mirrored.right_spectral_shape, atol=1e-6)


def test_native_python_shared_golden_vector() -> None:
    frames = 12000
    state = 0x12345678
    mono = np.zeros(frames + 64, dtype=np.float32)
    for index in range(len(mono)):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        noise = ((state >> 8) & 0xFFFF) / 32768.0 - 1.0
        mono[index] = (np.float32(0.15) * np.float32(noise) +
                       np.float32(0.12) * np.sin(
                           np.float32(index) * np.float32(0.17), dtype=np.float32
                       ))
    stereo = np.zeros((frames, 2), dtype=np.float32)
    stereo[7:, 0] = mono[:frames - 7]
    stereo[7:, 1] = mono[7:frames] * np.float32(1.35)
    features = extract_direction_features(stereo)
    assert np.isclose(features.broadband_ild_db, -2.69309, atol=0.03)
    assert np.isclose(features.gcc_delay_samples, 7.0, atol=0.03)
    assert np.isclose(features.gcc_peak, 0.99998, atol=0.03)
    assert np.isclose(features.gcc_peak_to_sidelobe, 0.99990, atol=0.20)
    assert np.isclose(features.active_frame_fraction, 0.52108, atol=0.02)
    assert np.isclose(features.rms, 0.15244, atol=0.01)
    assert np.isclose(features.band_ild_db[0], -2.55719, atol=0.08)
    assert np.isclose(features.band_coherence[0], 0.99974, atol=0.02)
    assert np.isclose(features.left_spectral_shape[0], 0.0014035, atol=0.0002)


def test_peak_selection_chooses_strongest_transient() -> None:
    samples = np.full((16000, 2), 0.0005, dtype=np.float32)
    phase = np.arange(240, dtype=np.float32)
    samples[2800:3040] = (0.08 * np.sin(phase * 0.31))[:, None]
    samples[9200:9440] = (0.55 * np.sin(phase * 0.29))[:, None]
    selected, peak = select_peak_window(
        samples, before_peak_ms=18, after_peak_ms=150
    )
    assert abs(peak.peak_frame - 9320) < 180
    assert peak.peak_to_noise_db > 20.0
    assert len(selected) == peak.end_frame - peak.start_frame


def test_peak_selection_rejects_non_peaky_audio() -> None:
    time = np.arange(12000, dtype=np.float32)
    tone = (0.03 * np.sin(time * 0.13))[:, None]
    samples = np.repeat(tone, 2, axis=1)
    with np.testing.assert_raises(ValueError):
        select_peak_window(samples, before_peak_ms=18, after_peak_ms=150)


def test_circular_error_wraps() -> None:
    assert circular_error_degrees(355.0, 5.0) == 10.0
    assert circular_error_degrees(90.0, 270.0) == 180.0


class _FakeRenderer:
    def render(self, samples: np.ndarray, parameters) -> np.ndarray:
        gain = 0.35 + 0.65 * (1.0 + np.sin(np.deg2rad(parameters.azimuth_degrees))) / 2.0
        result = np.array(samples, copy=True)
        result[:, 1] *= gain
        return result


def test_direction_corpus_writes_known_bearing_manifest(tmp_path: Path) -> None:
    source = tmp_path / "footsteps" / "step.wav"
    write_pcm16_wav(source, Audio(_stereo_fixture(), SAMPLE_RATE))
    output = tmp_path / "direction"
    rendered = generate_direction_corpus(
        [source], output, _FakeRenderer(), azimuths=(0.0, 90.0)
    )
    assert len(rendered) == 2
    lines = (output / "direction-manifest.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 2
    assert '"azimuth_degrees": 90.0' in lines[1]
    assert '"sound_class": "footstep"' in lines[0]
    assert '"condition": "clean"' in lines[0]
    assert '"source_split":' in lines[0]
    assert '"ground_truth_peak_sample"' in lines[0]

    rows = []
    for line in lines:
        payload = json.loads(line)
        payload["features"] = DirectionFeatures(**payload["features"])
        rows.append(DirectionCorpusRow(**payload))
    report = evaluate_direction_rows(rows)
    assert report.count == 2
    assert 0.0 <= report.confidence_ece <= 1.0
    assert np.isclose(
        report.peak_selection_success_rate + report.peak_rejection_rate, 1.0
    )


def test_direction_corpus_supports_controlled_conditions(tmp_path: Path) -> None:
    source = tmp_path / "gunshots" / "shot.wav"
    write_pcm16_wav(source, Audio(_stereo_fixture(), SAMPLE_RATE))
    rendered = generate_direction_corpus(
        [source], tmp_path / "direction", _FakeRenderer(), azimuths=(0.0,),
        conditions=("clean", "noise", "mild-reverb", "occlusion", "channel-isolation"),
    )
    assert len(rendered) == 5
