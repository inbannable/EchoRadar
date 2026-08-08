from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import json
from pathlib import Path
import random

import numpy as np

from . import CLASS_NAMES, CONTEXT_FRAMES, HOP_SIZE, SAMPLE_RATE
from .audio import Audio, load_pcm_wav, rms, to_stereo_48k, trim_activity, write_pcm16_wav
from .manifest import Asset, load_split_manifest
from .spatial import SpatialParameters, SteamAudioRenderer


@dataclass(frozen=True)
class TimelineEvent:
    event_id: str
    sound_class: str
    onset_sample: int
    end_sample: int
    source_asset: str
    source_group: str
    stratum: str
    snr_db: float
    overlap: bool
    seen_source: bool
    render_mode: str = "self"
    gain_db: float = 0.0
    distance_m: float = 0.0
    azimuth_quadrant: str = "center"
    occluded: bool = False
    source_family: str = ""

    def json_dict(self) -> dict[str, object]:
        values = asdict(self)
        values["class"] = values.pop("sound_class")
        return values


def _one_pole_lowpass(samples: np.ndarray, coefficient: float) -> np.ndarray:
    output = np.empty_like(samples)
    output[0] = samples[0]
    for index in range(1, len(samples)):
        output[index] = coefficient * samples[index] + (1.0 - coefficient) * output[index - 1]
    return output


def _augment(samples: np.ndarray, rng: random.Random, complex_scene: bool) -> np.ndarray:
    output = samples.astype(np.float32, copy=True)
    output *= 10.0 ** (rng.uniform(-9.0, 3.0) / 20.0)
    if complex_scene and rng.random() < 0.45:
        output = _one_pole_lowpass(output, rng.uniform(0.08, 0.65))
    if complex_scene and rng.random() < 0.35:
        decay_samples = rng.randint(240, 2400)
        impulse = np.power(rng.uniform(0.96, 0.998), np.arange(decay_samples, dtype=np.float32))
        impulse[0] = 1.0
        impulse *= rng.uniform(0.03, 0.20)
        impulse[0] = 1.0
        for channel in range(2):
            output[:, channel] = np.convolve(output[:, channel], impulse, mode="full")[: len(output)]
    if complex_scene and rng.random() < 0.30:
        drive = rng.uniform(1.0, 2.5)
        output = np.tanh(output * drive) / np.tanh(drive)
    return output.astype(np.float32)


def _background(assets: list[Asset], root: Path, frames: int, rng: random.Random) -> np.ndarray:
    output = np.zeros((frames, 2), dtype=np.float32)
    if not assets:
        return output
    cursor = 0
    while cursor < frames:
        asset = rng.choice(assets)
        clip = trim_activity(to_stereo_48k(load_pcm_wav(root / asset.relative_path))).samples
        if len(clip) == 0:
            continue
        take = min(len(clip), frames - cursor)
        gain = 10.0 ** (rng.uniform(-24.0, -10.0) / 20.0)
        output[cursor : cursor + take] += clip[:take] * gain
        cursor += take
        silence = min(rng.randint(0, SAMPLE_RATE // 2), frames - cursor)
        cursor += silence
    noise = np.random.default_rng(rng.randrange(2**32)).normal(0.0, 0.0015, output.shape)
    return output + noise.astype(np.float32)


def generate_session(
    assets: list[Asset],
    asset_root: str | Path,
    split: str,
    stratum: str,
    duration_seconds: float,
    seed: int,
    renderer: SteamAudioRenderer | None = None,
    ambient_only: bool = False,
    session_gain_db: float = 0.0,
) -> tuple[Audio, list[TimelineEvent]]:
    if stratum not in ("simple", "complex"):
        raise ValueError("stratum must be simple or complex")
    rng = random.Random(seed)
    selected = [asset for asset in assets if asset.split == split]
    by_label = {label: [asset for asset in selected if asset.label == label] for label in (*CLASS_NAMES, "other")}
    for label in (*CLASS_NAMES, "other"):
        if not by_label[label]:
            raise ValueError(f"split {split} has no {label} assets")

    total_frames = int(round(duration_seconds * SAMPLE_RATE))
    root = Path(asset_root)
    mix = _background(by_label["other"], root, total_frames, rng)
    background_rms = max(rms(mix), 1e-5)
    minimum_events = 5 if stratum == "complex" else len(CLASS_NAMES)
    event_count = 0 if ambient_only else max(
        minimum_events, int(duration_seconds * (0.16 if stratum == "simple" else 0.35))
    )
    events: list[TimelineEvent] = []
    occupied: list[tuple[int, int]] = []
    # Sequential session seeds alternate which event indices are rendered,
    # giving each fixed class exactly balanced self/remote coverage.
    remote_parity = seed & 1

    for index in range(event_count):
        # Every session covers all user-facing classes before sampling the
        # natural target mix. This keeps small locked suites conclusive.
        if index < len(CLASS_NAMES):
            label = CLASS_NAMES[index]
        elif stratum == "complex" and index in (3, 4):
            label = ("gunshot", "footstep")[index - 3]
        else:
            label = rng.choices(CLASS_NAMES, weights=(0.38, 0.42, 0.20), k=1)[0]
        asset = rng.choice(by_label[label])
        # Timeline onset is the first audible target sample, not the beginning
        # of any padding retained in the extracted asset.
        clip = trim_activity(to_stereo_48k(load_pcm_wav(root / asset.relative_path))).samples
        render_mode = "self"
        spatial = SpatialParameters(0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0)
        if renderer is not None and index % 2 == remote_parity:
            render_mode = "remote"
            occlusion = rng.uniform(0.15, 0.85) if rng.random() < 0.35 else 0.0
            spatial = SpatialParameters(
                azimuth_degrees=rng.uniform(-180.0, 180.0),
                elevation_degrees=rng.choice((-30.0, 0.0, 30.0)),
                distance_meters=10.0 ** rng.uniform(0.0, np.log10(25.0)),
                occlusion=occlusion,
                transmission_low=rng.uniform(0.35, 0.95),
                transmission_mid=rng.uniform(0.20, 0.85),
                transmission_high=rng.uniform(0.05, 0.70),
                directivity=rng.uniform(0.45, 1.0),
                reverb_mix=rng.uniform(0.0, 0.20),
            )
            clip = renderer.render(clip, spatial)
        clip = _augment(clip, rng, stratum == "complex")
        if len(clip) >= total_frames:
            clip = clip[: max(1, total_frames // 2)]

        target_snr = rng.uniform(10.0, 24.0) if stratum == "simple" else rng.uniform(-10.0, 20.0)
        clip_rms = max(rms(clip), 1e-6)
        clip *= background_rms * (10.0 ** (target_snr / 20.0)) / clip_rms

        warmup = CONTEXT_FRAMES * HOP_SIZE
        latest_start = max(warmup, total_frames - len(clip) - warmup)
        start = rng.randint(warmup, latest_start)
        if stratum == "complex" and index == 1 and occupied:
            first_start, first_end = occupied[0]
            overlap_offset = min(SAMPLE_RATE // 50, max(1, (first_end - first_start) // 2))
            start = min(latest_start, first_start + overlap_offset)
        elif stratum == "complex" and index in (3, 4) and len(events) >= index - 2:
            reference = events[index - 3]
            separation = (60, 100)[index - 3] * SAMPLE_RATE // 1000
            start = min(latest_start, reference.onset_sample + separation)
        elif stratum == "simple":
            for _ in range(100):
                if all(start + len(clip) + SAMPLE_RATE // 5 <= old_start or
                       start >= old_end + SAMPLE_RATE // 5 for old_start, old_end in occupied):
                    break
                start = rng.randint(warmup, latest_start)
        clip_end = start + len(clip)
        event_duration = int(0.05 * SAMPLE_RATE)
        event_end = start + min(len(clip), event_duration)
        overlapping_indices = [old_index for old_index, (old_start, old_end) in enumerate(occupied)
                               if start < old_end and event_end > old_start]
        overlap = bool(overlapping_indices)
        for old_index in overlapping_indices:
            events[old_index] = replace(events[old_index], overlap=True)
        mix[start:clip_end] += clip
        occupied.append((start, event_end))
        events.append(TimelineEvent(
            event_id=f"{split}_{stratum}_{seed}_{index:05d}", sound_class=label,
            onset_sample=start, end_sample=event_end, source_asset=asset.relative_path,
            source_group=asset.source_group, stratum=stratum,
            snr_db=round(target_snr, 3), overlap=overlap, seen_source=split != "test",
            render_mode=render_mode, gain_db=round(session_gain_db, 3),
            distance_m=round(spatial.distance_meters, 3),
            azimuth_quadrant=spatial.azimuth_quadrant if render_mode == "remote" else "center",
            occluded=spatial.occlusion > 0.0, source_family=asset.source_group,
        ))

    peak = float(np.max(np.abs(mix))) if mix.size else 0.0
    if peak > 0.98:
        mix *= 0.98 / peak
    mix *= 10.0 ** (session_gain_db / 20.0)
    mix = np.clip(mix, -1.0, 1.0)
    return Audio(mix.astype(np.float32), SAMPLE_RATE), sorted(events, key=lambda event: event.onset_sample)


def write_session(output_prefix: str | Path, audio: Audio, timeline: list[TimelineEvent], metadata: dict) -> None:
    prefix = Path(output_prefix)
    write_pcm16_wav(prefix.with_suffix(".wav"), audio)
    with prefix.with_suffix(".jsonl").open("w", encoding="utf-8") as stream:
        for event in timeline:
            stream.write(json.dumps(event.json_dict(), sort_keys=True) + "\n")
    with prefix.with_suffix(".session.json").open("w", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2, sort_keys=True)
        stream.write("\n")


def generate_from_manifest(
    split_manifest: str | Path,
    asset_root: str | Path,
    output_dir: str | Path,
    split: str,
    stratum: str,
    count: int,
    duration_seconds: float,
    seed: int,
    renderer: SteamAudioRenderer | None = None,
    ambient_only_fraction: float = 0.0,
    session_gain_min_db: float = 0.0,
    session_gain_max_db: float = 0.0,
) -> list[Path]:
    assets = load_split_manifest(split_manifest)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    if not (0.0 <= ambient_only_fraction <= 1.0):
        raise ValueError("ambient_only_fraction must be between zero and one")
    if session_gain_min_db > session_gain_max_db:
        raise ValueError("session gain range is inverted")
    ambient_count = int(round(count * ambient_only_fraction))
    ambient_indices = set(random.Random(seed ^ 0x51A7E).sample(range(count), ambient_count))
    for index in range(count):
        session_seed = seed + index
        session_rng = random.Random(session_seed ^ 0xA5A55A5A)
        ambient_only = index in ambient_indices
        session_gain_db = session_rng.uniform(session_gain_min_db, session_gain_max_db)
        audio, timeline = generate_session(
            assets, asset_root, split, stratum, duration_seconds, session_seed,
            renderer=renderer, ambient_only=ambient_only, session_gain_db=session_gain_db,
        )
        prefix = output / f"{split}_{stratum}_{session_seed}"
        write_session(prefix, audio, timeline, {
            "generator_version": "mixture-v3", "seed": session_seed, "split": split,
            "stratum": stratum, "sample_rate": SAMPLE_RATE, "duration_seconds": duration_seconds,
            "ambient_only": ambient_only, "session_gain_db": round(session_gain_db, 3),
            "renderer": "steam-audio-v4.8.1" if renderer is not None else "self",
        })
        paths.append(prefix)
    return paths
