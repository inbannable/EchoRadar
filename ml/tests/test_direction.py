from pathlib import Path

import numpy as np

from echoradar_ml import SAMPLE_RATE
from echoradar_ml.audio import Audio, write_pcm16_wav
from echoradar_ml.direction import (
    circular_error_degrees,
    extract_direction_features,
    generate_direction_corpus,
)


def _stereo_fixture(frames: int = 4096) -> np.ndarray:
    time = np.arange(frames, dtype=np.float32) / SAMPLE_RATE
    tone = np.sin(2.0 * np.pi * 1200.0 * time).astype(np.float32)
    samples = np.zeros((frames, 2), dtype=np.float32)
    samples[:, 0] = tone
    samples[3:, 1] = tone[:-3] * 0.45
    return samples


def test_channel_swap_mirrors_pooled_cues() -> None:
    original = extract_direction_features(_stereo_fixture())
    mirrored = extract_direction_features(_stereo_fixture()[:, ::-1])
    assert np.isclose(original.broadband_ild_db, -mirrored.broadband_ild_db, atol=0.01)
    assert np.isclose(original.itd_samples, -mirrored.itd_samples, atol=0.01)


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
