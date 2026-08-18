from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import hashlib
import json
import math
from pathlib import Path
import random
from typing import Iterable, Protocol

import numpy as np

from . import CLASS_NAMES, FFT_SIZE, HOP_SIZE, SAMPLE_RATE
from .audio import Audio, load_pcm_wav, rms, to_stereo_48k, write_pcm16_wav
from .manifest import Asset, assert_no_leakage, load_split_manifest
from .spatial import STEAM_AUDIO_VERSION, SpatialParameters


DIRECTION_SCENE_SCHEMA = "direction-scenes-v1"
DIRECTION_REAL_SCENE_SCHEMA = "direction-real-scenes-v1"
DIRECTION_SCENE_FRAMES = 48
DIRECTION_SCENE_SAMPLES = FFT_SIZE + (DIRECTION_SCENE_FRAMES - 1) * HOP_SIZE
DIRECTION_SCENE_MILLISECONDS = DIRECTION_SCENE_SAMPLES * 1000.0 / SAMPLE_RATE
DIRECTION_PRE_ANCHOR_MILLISECONDS = 40.0
DIRECTION_JOIN_MILLISECONDS = 120.0
MINIMUM_SOURCE_SEPARATION_DEGREES = 15.0
CLOSE_SOURCE_MAXIMUM_DEGREES = 30.0
FULL_DIRECTION_COUNTS = {"train": 100_000, "dev": 10_000, "test": 20_000}
SMOKE_DIRECTION_COUNTS = {"train": 2_000, "dev": 250, "test": 500}


class SceneRenderer(Protocol):
    def render(self, samples: np.ndarray, parameters: SpatialParameters) -> np.ndarray: ...


@dataclass(frozen=True)
class SceneDirection:
    azimuth_degrees: float
    elevation_degrees: float


@dataclass(frozen=True)
class DirectionSceneSource:
    source_id: str
    source_asset: str
    source_sha256: str
    source_group: str
    sound_class: str
    onset_sample: int
    onset_milliseconds: float
    gain_db: float
    world_direction: SceneDirection
    rendered_direction: SceneDirection
    distance_meters: float
    occlusion: float
    transmission_low: float
    transmission_mid: float
    transmission_high: float
    directivity: float
    reverb_mix: float


@dataclass(frozen=True)
class DirectionSceneDistractor:
    source_id: str
    source_asset: str
    source_sha256: str
    source_group: str
    negative_subtype: str
    onset_sample: int
    gain_db: float
    rendered_direction: SceneDirection
    distance_meters: float
    occlusion: float
    transmission_low: float
    transmission_mid: float
    transmission_high: float
    directivity: float
    reverb_mix: float


@dataclass(frozen=True)
class DirectionSceneRow:
    schema_version: str
    scene_id: str
    scene_seed: int
    split: str
    stratum: str
    relative_path: str
    wav_sha256: str
    renderer_version: str
    sample_rate: int
    sample_count: int
    feature_frames: int
    fft_size: int
    hop_size: int
    target_count: int
    target_class_combination: str
    coincident_onsets: bool
    background_snr_db: float
    capture_gain_db: float
    final_anticlip_gain_db: float
    profile: dict[str, object]
    targets: tuple[DirectionSceneSource, ...]
    distractors: tuple[DirectionSceneDistractor, ...]

    def json_dict(self) -> dict[str, object]:
        return asdict(self)


def great_circle_degrees(left: SceneDirection, right: SceneDirection) -> float:
    left_vector = _direction_vector(left)
    right_vector = _direction_vector(right)
    return math.degrees(math.acos(float(np.clip(np.dot(left_vector, right_vector), -1.0, 1.0))))


def _direction_vector(direction: SceneDirection) -> np.ndarray:
    azimuth = math.radians(direction.azimuth_degrees)
    elevation = math.radians(direction.elevation_degrees)
    horizontal = math.cos(elevation)
    return np.asarray(
        [horizontal * math.sin(azimuth), math.sin(elevation), horizontal * math.cos(azimuth)],
        dtype=np.float64,
    )


def _vector_direction(vector: np.ndarray) -> SceneDirection:
    unit = vector / max(1.0e-12, float(np.linalg.norm(vector)))
    return SceneDirection(
        azimuth_degrees=float(math.degrees(math.atan2(unit[0], unit[2])) % 360.0),
        elevation_degrees=float(math.degrees(math.atan2(unit[1], math.hypot(unit[0], unit[2])))),
    )


def _nearby_direction(origin: SceneDirection, separation: float, bearing: float) -> SceneDirection:
    vector = _direction_vector(origin)
    reference = np.asarray([0.0, 1.0, 0.0], dtype=np.float64)
    if abs(float(np.dot(vector, reference))) > 0.95:
        reference = np.asarray([1.0, 0.0, 0.0], dtype=np.float64)
    tangent_a = np.cross(reference, vector)
    tangent_a /= np.linalg.norm(tangent_a)
    tangent_b = np.cross(vector, tangent_a)
    tangent = math.cos(bearing) * tangent_a + math.sin(bearing) * tangent_b
    return _vector_direction(
        math.cos(math.radians(separation)) * vector +
        math.sin(math.radians(separation)) * tangent
    )


def sample_scene_directions(
    rng: random.Random,
    count: int,
    close_source: bool = False,
) -> tuple[SceneDirection, ...]:
    if not 0 <= count <= 3:
        raise ValueError("direction scenes support zero to three targets")
    if close_source and count < 2:
        raise ValueError("the close-source stratum requires at least two targets")
    result: list[SceneDirection] = []
    if close_source:
        for _ in range(2_000):
            first = SceneDirection(rng.random() * 360.0, rng.uniform(-60.0, 60.0))
            second = _nearby_direction(
                first, rng.uniform(MINIMUM_SOURCE_SEPARATION_DEGREES, CLOSE_SOURCE_MAXIMUM_DEGREES),
                rng.uniform(0.0, 2.0 * math.pi),
            )
            if -60.0 <= second.elevation_degrees <= 60.0:
                result.extend((first, second))
                break
        if not result:
            raise RuntimeError("could not sample a bounded close-source pair")
    while len(result) < count:
        for _ in range(2_000):
            candidate = SceneDirection(rng.random() * 360.0, rng.uniform(-60.0, 60.0))
            if all(great_circle_degrees(candidate, old) >= MINIMUM_SOURCE_SEPARATION_DEGREES
                   for old in result):
                result.append(candidate)
                break
        else:
            raise RuntimeError("could not sample separated source directions")
    return tuple(result)


def _proportional_counts(total: int, proportions: tuple[float, ...]) -> tuple[int, ...]:
    exact = np.asarray(proportions, dtype=np.float64) * total
    counts = np.floor(exact).astype(np.int64)
    remainder = total - int(counts.sum())
    order = sorted(range(len(proportions)), key=lambda index: (-(exact[index] - counts[index]), index))
    for index in order[:remainder]:
        counts[index] += 1
    return tuple(int(value) for value in counts)


def target_count_schedule(total: int, seed: int) -> tuple[int, ...]:
    if total <= 0:
        raise ValueError("scene count must be positive")
    counts = _proportional_counts(total, (0.10, 0.15, 0.35, 0.40))
    values = [target_count for target_count, count in enumerate(counts) for _ in range(count)]
    random.Random(seed ^ 0xC01A7).shuffle(values)
    return tuple(values)


def target_class_schedule(target_counts: Iterable[int], seed: int) -> list[tuple[str, ...]]:
    combinations = {
        0: ((),),
        1: (("footstep",), ("gunshot",)),
        2: (("footstep", "footstep"), ("gunshot", "gunshot"), ("footstep", "gunshot")),
        3: (
            ("footstep", "footstep", "footstep"),
            ("gunshot", "gunshot", "gunshot"),
            ("footstep", "footstep", "gunshot"),
            ("footstep", "gunshot", "gunshot"),
        ),
    }
    occurrences = {count: 0 for count in combinations}
    offsets = {count: random.Random(seed + count * 104729).randrange(len(values))
               for count, values in combinations.items()}
    output: list[tuple[str, ...]] = []
    for count in target_counts:
        choices = combinations[count]
        selection = choices[(occurrences[count] + offsets[count]) % len(choices)]
        occurrences[count] += 1
        output.append(selection)
    return output


def _spatial_parameters(rng: random.Random, direction: SceneDirection) -> SpatialParameters:
    occlusion = rng.uniform(0.05, 0.95) if rng.random() < 0.40 else 0.0
    return SpatialParameters(
        azimuth_degrees=direction.azimuth_degrees,
        elevation_degrees=direction.elevation_degrees,
        distance_meters=10.0 ** rng.uniform(0.0, math.log10(40.0)),
        occlusion=occlusion,
        transmission_low=rng.uniform(0.35, 1.0) if occlusion else 1.0,
        transmission_mid=rng.uniform(0.15, 0.95) if occlusion else 1.0,
        transmission_high=rng.uniform(0.03, 0.80) if occlusion else 1.0,
        directivity=rng.uniform(0.0, 1.0),
        reverb_mix=rng.uniform(0.0, 0.25),
    )


def _load_asset(root: Path, asset: Asset) -> tuple[np.ndarray, int]:
    samples = to_stereo_48k(load_pcm_wav(root / asset.relative_path)).samples
    if not len(samples):
        raise ValueError(f"empty direction source asset: {asset.relative_path}")
    onset = asset.active_offset_samples
    if onset < 0 or onset >= len(samples):
        envelope = np.max(np.abs(samples), axis=1)
        threshold = max(1.0e-5, float(envelope.max()) * 0.05)
        active = np.flatnonzero(envelope >= threshold)
        onset = int(active[0]) if len(active) else 0
    return samples, onset


def _fit_rendered_source(rendered: np.ndarray, source_onset: int, scene_onset: int) -> np.ndarray:
    stereo = np.asarray(rendered, dtype=np.float32)
    if stereo.ndim != 2 or stereo.shape[1] != 2:
        raise ValueError("direction renderer must return [frames, 2] stereo")
    if not len(stereo) or not np.isfinite(stereo).all():
        raise ValueError("direction renderer returned empty or non-finite stereo")
    output = np.zeros((DIRECTION_SCENE_SAMPLES, 2), dtype=np.float32)
    destination_start = scene_onset - source_onset
    source_start = max(0, -destination_start)
    destination_start = max(0, destination_start)
    take = min(len(stereo) - source_start, len(output) - destination_start)
    if take > 0:
        output[destination_start:destination_start + take] = stereo[source_start:source_start + take]
    return output


def _deterministic_lowpass(samples: np.ndarray, coefficient: float) -> np.ndarray:
    output = np.empty_like(samples, dtype=np.float32)
    if not len(samples):
        return output
    # A causal moving-average approximation keeps corpus generation O(samples)
    # and uses a fixed cumsum order, preserving repeatable WAV hashes.
    window = int(np.clip(round(2.0 / max(coefficient, 0.02)), 3, 64))
    ends = np.arange(1, len(samples) + 1, dtype=np.int64)
    starts = np.maximum(0, ends - window)
    counts = (ends - starts).astype(np.float64)
    for channel in range(samples.shape[1]):
        prefix = np.concatenate((
            np.zeros(1, dtype=np.float64),
            np.cumsum(samples[:, channel], dtype=np.float64),
        ))
        output[:, channel] = ((prefix[ends] - prefix[starts]) / counts).astype(np.float32)
    return output


def _apply_profile(samples: np.ndarray, profile: dict[str, object]) -> np.ndarray:
    output = samples.astype(np.float32, copy=True)
    eq = str(profile["eq_profile"])
    if eq == "smooth":
        output = 0.25 * output + 0.75 * _deterministic_lowpass(output, 0.28)
    elif eq == "crisp":
        low = _deterministic_lowpass(output, 0.35)
        output = output + 0.20 * (output - low)
    isolation = float(profile["left_right_isolation_percent"]) / 100.0
    mid = 0.5 * (output[:, 0] + output[:, 1])
    side = 0.5 * (output[:, 0] - output[:, 1]) * isolation
    output[:, 0] = mid + side
    output[:, 1] = mid - side
    return output.astype(np.float32)


def _asset_choice(rng: random.Random, assets: list[Asset], used: set[str]) -> Asset:
    candidates = [asset for asset in assets if asset.asset_id not in used] or assets
    asset = rng.choice(candidates)
    used.add(asset.asset_id)
    return asset


def generate_direction_scene(
    assets: list[Asset],
    asset_root: str | Path,
    split: str,
    scene_index: int,
    seed: int,
    target_classes: tuple[str, ...],
    renderer: SceneRenderer,
    close_source: bool = False,
    coincident_onsets: bool | None = None,
) -> tuple[Audio, DirectionSceneRow]:
    if any(name not in CLASS_NAMES for name in target_classes) or len(target_classes) > 3:
        raise ValueError("direction scene target classes must be footsteps or gunshots")
    selected = [asset for asset in assets if asset.included and not asset.duplicate_of
                and asset.split == split]
    by_class = {name: [asset for asset in selected if asset.label == name] for name in CLASS_NAMES}
    negatives = [asset for asset in selected if not asset.is_target]
    for name in set(target_classes):
        if not by_class[name]:
            raise ValueError(f"split {split} has no {name} direction assets")
    if not negatives:
        raise ValueError(f"split {split} has no spatial distractors")

    scene_seed = seed + scene_index
    rng = random.Random(scene_seed)
    numpy_rng = np.random.default_rng(scene_seed ^ 0xD1EC710)
    directions = sample_scene_directions(rng, len(target_classes), close_source)
    coincident = len(target_classes) >= 2 and (
        scene_index % 2 == 0 if coincident_onsets is None else coincident_onsets
    )
    base_onset = int(DIRECTION_PRE_ANCHOR_MILLISECONDS * SAMPLE_RATE / 1000.0)
    if coincident or len(target_classes) < 2:
        onsets = [base_onset] * len(target_classes)
    else:
        onsets = [base_onset + rng.randint(1, 80 * SAMPLE_RATE // 1000)
                  for _ in target_classes]
        onsets[0] = base_onset

    profile: dict[str, object] = {
        "eq_profile": rng.choice(("natural", "crisp", "smooth")),
        "left_right_isolation_percent": round(rng.uniform(35.0, 100.0), 3),
        # Offline Steam Audio renders use listener-relative acoustic bearings.
        # Perspective-corrected labels are collected only from real CS2, where
        # the corresponding camera ray can be recorded and audited.
        "perspective_correction": False,
        "output_profile": f"synthetic-{rng.randrange(4)}",
    }
    mix = np.zeros((DIRECTION_SCENE_SAMPLES, 2), dtype=np.float32)
    target_rows: list[DirectionSceneSource] = []
    distractor_rows: list[DirectionSceneDistractor] = []
    used_assets: set[str] = set()

    for source_index, (sound_class, direction, scene_onset) in enumerate(
        zip(target_classes, directions, onsets, strict=True)
    ):
        asset = _asset_choice(rng, by_class[sound_class], used_assets)
        source, source_onset = _load_asset(Path(asset_root), asset)
        parameters = _spatial_parameters(rng, direction)
        rendered = renderer.render(source, parameters)
        gain_db = rng.uniform(-12.0, 3.0)
        mix += _fit_rendered_source(rendered, source_onset, scene_onset) * np.float32(
            10.0 ** (gain_db / 20.0)
        )
        target_rows.append(DirectionSceneSource(
            source_id=f"target-{source_index}", source_asset=asset.relative_path,
            source_sha256=asset.sha256, source_group=asset.source_group,
            sound_class=sound_class, onset_sample=scene_onset,
            onset_milliseconds=round(scene_onset * 1000.0 / SAMPLE_RATE, 4),
            gain_db=round(gain_db, 4), world_direction=direction,
            rendered_direction=direction, distance_meters=round(parameters.distance_meters, 4),
            occlusion=round(parameters.occlusion, 5),
            transmission_low=round(parameters.transmission_low, 5),
            transmission_mid=round(parameters.transmission_mid, 5),
            transmission_high=round(parameters.transmission_high, 5),
            directivity=round(parameters.directivity, 5), reverb_mix=round(parameters.reverb_mix, 5),
        ))

    distractor_count = rng.choices((0, 1, 2), weights=(0.30, 0.45, 0.25), k=1)[0]
    for source_index in range(distractor_count):
        asset = _asset_choice(rng, negatives, used_assets)
        source, source_onset = _load_asset(Path(asset_root), asset)
        direction = SceneDirection(rng.random() * 360.0, rng.uniform(-60.0, 60.0))
        parameters = _spatial_parameters(rng, direction)
        onset = rng.randint(0, max(0, 120 * SAMPLE_RATE // 1000))
        gain_db = rng.uniform(-24.0, -5.0)
        rendered = renderer.render(source, parameters)
        mix += _fit_rendered_source(rendered, source_onset, onset) * np.float32(
            10.0 ** (gain_db / 20.0)
        )
        distractor_rows.append(DirectionSceneDistractor(
            source_id=f"distractor-{source_index}", source_asset=asset.relative_path,
            source_sha256=asset.sha256, source_group=asset.source_group,
            negative_subtype=asset.subtype or asset.label, onset_sample=onset,
            gain_db=round(gain_db, 4), rendered_direction=direction,
            distance_meters=round(parameters.distance_meters, 4),
            occlusion=round(parameters.occlusion, 5),
            transmission_low=round(parameters.transmission_low, 5),
            transmission_mid=round(parameters.transmission_mid, 5),
            transmission_high=round(parameters.transmission_high, 5),
            directivity=round(parameters.directivity, 5), reverb_mix=round(parameters.reverb_mix, 5),
        ))

    background_snr_db = rng.uniform(3.0, 30.0)
    foreground_rms = max(rms(mix), 1.0e-5)
    noise_rms = foreground_rms / (10.0 ** (background_snr_db / 20.0))
    if not target_classes:
        noise_rms = 10.0 ** (rng.uniform(-54.0, -32.0) / 20.0)
    noise = numpy_rng.normal(0.0, noise_rms, mix.shape).astype(np.float32)
    # A shared low-frequency component is a simple deterministic room/capture bed.
    noise = 0.75 * noise + 0.25 * _deterministic_lowpass(noise, 0.06)
    mix += noise.astype(np.float32)
    mix = _apply_profile(mix, profile)
    capture_gain_db = rng.uniform(-18.0, 6.0)
    mix *= np.float32(10.0 ** (capture_gain_db / 20.0))
    if not np.isfinite(mix).all():
        raise ValueError("direction scene mix became non-finite")
    peak = float(np.max(np.abs(mix))) if mix.size else 0.0
    anticlipping = min(1.0, 0.98 / peak) if peak > 0.0 else 1.0
    mix *= np.float32(anticlipping)
    anticlipping_db = 20.0 * math.log10(max(anticlipping, 1.0e-12))

    scene_id = f"{split}-{seed}-{scene_index:06d}"
    relative_path = f"{split}/{scene_id}.wav"
    row = DirectionSceneRow(
        schema_version=DIRECTION_SCENE_SCHEMA, scene_id=scene_id, scene_seed=scene_seed,
        split=split, stratum="close-15-30" if close_source else "standard",
        relative_path=relative_path, wav_sha256="", renderer_version=STEAM_AUDIO_VERSION,
        sample_rate=SAMPLE_RATE, sample_count=DIRECTION_SCENE_SAMPLES,
        feature_frames=DIRECTION_SCENE_FRAMES, fft_size=FFT_SIZE, hop_size=HOP_SIZE,
        target_count=len(target_rows), target_class_combination="+".join(target_classes) or "none",
        coincident_onsets=coincident, background_snr_db=round(background_snr_db, 4),
        capture_gain_db=round(capture_gain_db, 4),
        final_anticlip_gain_db=round(anticlipping_db, 4), profile=profile,
        targets=tuple(target_rows), distractors=tuple(distractor_rows),
    )
    return Audio(mix.astype(np.float32), SAMPLE_RATE), row


def generate_direction_mixtures(
    split_manifest: str | Path,
    asset_root: str | Path,
    output_dir: str | Path,
    renderer: SceneRenderer,
    counts: dict[str, int] | None = None,
    seed: int = 20260720,
    close_test_fraction: float = 0.20,
) -> Path:
    counts = dict(FULL_DIRECTION_COUNTS if counts is None else counts)
    if set(counts) != {"train", "dev", "test"} or any(value <= 0 for value in counts.values()):
        raise ValueError("direction mixture counts must contain positive train/dev/test values")
    if not 0.0 <= close_test_fraction <= 1.0:
        raise ValueError("close-test fraction must be between zero and one")
    assets = load_split_manifest(split_manifest)
    assert_no_leakage(assets)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "direction-scenes.jsonl"
    temporary_manifest = manifest.with_suffix(".jsonl.partial")
    with temporary_manifest.open("w", encoding="utf-8") as stream:
        split_offset = 0
        for split_index, split in enumerate(("train", "dev", "test")):
            count = counts[split]
            split_seed = seed + split_index * 10_000_000
            target_counts = target_count_schedule(count, split_seed)
            class_schedules = target_class_schedule(target_counts, split_seed)
            multi_indices = [index for index, value in enumerate(target_counts) if value >= 2]
            coincident_count = len(multi_indices) // 2
            coincident_indices = set(random.Random(
                split_seed ^ 0xC01AC1DE
            ).sample(multi_indices, coincident_count))
            close_count = int(round(count * close_test_fraction)) if split == "test" else 0
            close_count = min(close_count, len(multi_indices))
            close_indices = set(random.Random(split_seed ^ 0xC105E).sample(multi_indices, close_count))
            (output / split).mkdir(parents=True, exist_ok=True)
            for index, classes in enumerate(class_schedules):
                audio, row = generate_direction_scene(
                    assets, asset_root, split, index + split_offset, split_seed, classes,
                    renderer, close_source=index in close_indices,
                    coincident_onsets=index in coincident_indices,
                )
                destination = output / row.relative_path
                write_pcm16_wav(destination, audio)
                digest = hashlib.sha256(destination.read_bytes()).hexdigest()
                row = replace(row, wav_sha256=digest)
                stream.write(json.dumps(row.json_dict(), sort_keys=True) + "\n")
            split_offset += count
    temporary_manifest.replace(manifest)
    summary = {
        "schema_version": DIRECTION_SCENE_SCHEMA,
        "seed": seed,
        "counts": counts,
        "scene_samples": DIRECTION_SCENE_SAMPLES,
        "scene_milliseconds": DIRECTION_SCENE_MILLISECONDS,
        "pre_anchor_milliseconds": DIRECTION_PRE_ANCHOR_MILLISECONDS,
        "join_milliseconds": DIRECTION_JOIN_MILLISECONDS,
        "feature_frames": DIRECTION_SCENE_FRAMES,
        "renderer_version": STEAM_AUDIO_VERSION,
        "close_test_fraction": close_test_fraction,
        "coincident_multi_source_policy": "deterministic-half-floor",
    }
    (output / "direction-scenes.summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def load_direction_scene_rows(path: str | Path, allow_real: bool = False) -> list[dict]:
    rows = [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines() if line]
    seen_scene_ids: set[str] = set()
    seen_paths: set[str] = set()
    for row in rows:
        schema = row.get("schema_version")
        if schema != DIRECTION_SCENE_SCHEMA and not (
            allow_real and schema == DIRECTION_REAL_SCENE_SCHEMA
        ):
            raise ValueError("incompatible direction scene manifest schema")
        if schema == DIRECTION_SCENE_SCHEMA and row.get("renderer_version") != STEAM_AUDIO_VERSION:
            raise ValueError("direction scene renderer version is not Steam Audio v4.8.1")
        if (row.get("sample_rate"), row.get("sample_count"), row.get("feature_frames")) != (
            SAMPLE_RATE, DIRECTION_SCENE_SAMPLES, DIRECTION_SCENE_FRAMES
        ):
            raise ValueError("direction scene preprocessing contract is incompatible")
        scene_id = str(row.get("scene_id", ""))
        relative_text = str(row.get("relative_path", ""))
        relative_path = Path(relative_text)
        if not scene_id or scene_id in seen_scene_ids:
            raise ValueError("direction scene IDs must be non-empty and unique")
        if (not relative_text or relative_text in seen_paths or relative_path.is_absolute()
                or ".." in relative_path.parts):
            raise ValueError("direction scene WAV paths must be unique package-relative paths")
        digest = str(row.get("wav_sha256", ""))
        if len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
            raise ValueError("direction scene WAV SHA-256 is invalid")
        if row.get("split") not in ("train", "dev", "test"):
            raise ValueError("direction scene split is invalid")
        targets = row.get("targets")
        distractors = row.get("distractors", [])
        if (not isinstance(targets, list) or not isinstance(distractors, list)
                or row.get("target_count") != len(targets) or not 0 <= len(targets) <= 3):
            raise ValueError("direction scene target/distractor structure is invalid")
        row.setdefault("distractors", distractors)
        directions: list[SceneDirection] = []
        for target in targets:
            if target.get("sound_class") not in CLASS_NAMES:
                raise ValueError("direction scene contains an unsupported target class")
            rendered = target.get("rendered_direction")
            if not isinstance(rendered, dict):
                raise ValueError("direction scene rendered direction is missing")
            try:
                direction = SceneDirection(
                    float(rendered["azimuth_degrees"]),
                    float(rendered["elevation_degrees"]),
                )
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError("direction scene rendered direction is invalid") from error
            if (not math.isfinite(direction.azimuth_degrees)
                    or not 0.0 <= direction.azimuth_degrees < 360.0
                    or not math.isfinite(direction.elevation_degrees)
                    or not -60.0 <= direction.elevation_degrees <= 60.0):
                raise ValueError("direction scene rendered direction is outside package bounds")
            directions.append(direction)
        if schema == DIRECTION_SCENE_SCHEMA:
            if row.get("stratum") not in ("standard", "close-15-30"):
                raise ValueError("synthetic direction scene stratum is invalid")
            pairwise_separations: list[float] = []
            for left in range(len(directions)):
                for right in range(left + 1, len(directions)):
                    separation = great_circle_degrees(directions[left], directions[right])
                    pairwise_separations.append(separation)
                    if separation < (
                        MINIMUM_SOURCE_SEPARATION_DEGREES - 1.0e-6
                    ):
                        raise ValueError("synthetic direction targets violate minimum separation")
            if row.get("stratum") == "close-15-30" and (
                not pairwise_separations or min(pairwise_separations) > (
                    CLOSE_SOURCE_MAXIMUM_DEGREES + 1.0e-6
                )
            ):
                raise ValueError("synthetic close-source stratum has no 15-30 degree pair")
        seen_scene_ids.add(scene_id)
        seen_paths.add(relative_text)
    return rows
