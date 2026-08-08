from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import json
from pathlib import Path
import random

import numpy as np

from . import CLASS_NAMES, SAMPLE_RATE
from .audio import (
    Audio, load_pcm_wav, rms, to_stereo_48k, trim_activity_with_offset, write_pcm16_wav,
)
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
    source_hint: str = "unknown"
    negative_subtype: str = ""
    render_mode: str = "self"
    gain_db: float = 0.0
    distance_m: float = 0.0
    azimuth_quadrant: str = "center"
    occluded: bool = False
    source_family: str = ""
    scene_mode: str = "quiet"
    reviewed: bool = True
    uncertain: bool = False

    def json_dict(self) -> dict[str, object]:
        values = asdict(self)
        values["class"] = values.pop("sound_class")
        return values


def _one_pole_lowpass(samples: np.ndarray, coefficient: float) -> np.ndarray:
    output = np.empty_like(samples)
    if not len(samples):
        return output
    output[0] = samples[0]
    for index in range(1, len(samples)):
        output[index] = coefficient * samples[index] + (1.0 - coefficient) * output[index - 1]
    return output


def _augment(samples: np.ndarray, rng: random.Random, complex_scene: bool) -> np.ndarray:
    output = samples.astype(np.float32, copy=True)
    output *= 10.0 ** (rng.uniform(-9.0, 3.0) / 20.0)
    if complex_scene and rng.random() < 0.55:
        output = _one_pole_lowpass(output, rng.uniform(0.08, 0.72))
    if complex_scene and rng.random() < 0.45:
        decay_samples = rng.randint(240, 2880)
        impulse = np.power(rng.uniform(0.96, 0.998), np.arange(decay_samples, dtype=np.float32))
        impulse *= rng.uniform(0.03, 0.20)
        impulse[0] = 1.0
        for channel in range(2):
            output[:, channel] = np.convolve(output[:, channel], impulse, mode="full")[:len(output)]
    if complex_scene and rng.random() < 0.40:
        drive = rng.uniform(1.0, 2.8)
        output = np.tanh(output * drive) / np.tanh(drive)
    if rng.random() < 0.35:
        # Small capture-chain mismatch without inventing a new direction.
        channel_gain = 10.0 ** (rng.uniform(-1.5, 1.5) / 20.0)
        output[:, rng.randrange(2)] *= channel_gain
    return output.astype(np.float32)


def _load_trimmed(root: Path, asset: Asset) -> tuple[np.ndarray, int]:
    audio = to_stereo_48k(load_pcm_wav(root / asset.relative_path))
    trimmed, offset = trim_activity_with_offset(audio)
    return trimmed.samples, offset


def _background(assets: list[Asset], root: Path, frames: int, rng: random.Random,
                complex_scene: bool) -> np.ndarray:
    output = np.zeros((frames, 2), dtype=np.float32)
    cursor = 0
    while assets and cursor < frames:
        asset = rng.choice(assets)
        clip, _ = _load_trimmed(root, asset)
        if not len(clip):
            continue
        take = min(len(clip), frames - cursor)
        gain_range = (-22.0, -7.0) if complex_scene else (-34.0, -18.0)
        output[cursor:cursor + take] += clip[:take] * 10.0 ** (rng.uniform(*gain_range) / 20.0)
        cursor += take
        cursor += min(rng.randint(0, SAMPLE_RATE // 2), frames - cursor)
    noise_scale = 0.0020 if complex_scene else 0.00045
    noise = np.random.default_rng(rng.randrange(2**32)).normal(0.0, noise_scale, output.shape)
    return output + noise.astype(np.float32)


def _spatial_parameters(rng: random.Random) -> SpatialParameters:
    occlusion = rng.uniform(0.15, 0.85) if rng.random() < 0.35 else 0.0
    return SpatialParameters(
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


def _place_clip(mix: np.ndarray, clip: np.ndarray, onset_offset: int, desired_onset: int,
                target_snr: float) -> tuple[int, int, float]:
    start = desired_onset - onset_offset
    if start < 0 or start + len(clip) > len(mix):
        raise ValueError("clip placement does not fit session")
    context = SAMPLE_RATE // 4
    local = mix[max(0, desired_onset - context):min(len(mix), desired_onset + context)]
    local_rms = max(rms(local), 1e-5)
    active = clip[onset_offset:min(len(clip), onset_offset + SAMPLE_RATE // 2)]
    clip_rms = max(rms(active), 1e-6)
    scaled = clip * (local_rms * 10.0 ** (target_snr / 20.0) / clip_rms)
    end = start + len(scaled)
    mix[start:end] += scaled
    achieved = 20.0 * np.log10(max(rms(scaled), 1e-9) / local_rms)
    return start, end, float(achieved)


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
    selected = [asset for asset in assets if asset.split == split and asset.included]
    by_label = {label: [asset for asset in selected if asset.label == label] for label in CLASS_NAMES}
    negatives = [asset for asset in selected if not asset.is_target]
    for label in CLASS_NAMES:
        if not by_label[label]:
            raise ValueError(f"split {split} has no {label} assets")
    if not negatives:
        raise ValueError(f"split {split} has no negative assets")

    total_frames = int(round(duration_seconds * SAMPLE_RATE))
    if total_frames < SAMPLE_RATE * 3:
        raise ValueError("sessions must be at least three seconds")
    root = Path(asset_root)
    complex_scene = stratum == "complex"
    mix = _background(negatives, root, total_frames, rng, complex_scene)
    target_count = 0 if ambient_only else max(6, int(duration_seconds * (0.18 if not complex_scene else 0.42)))
    negative_count = max(2, int(duration_seconds * (0.10 if not complex_scene else 0.28)))
    events: list[TimelineEvent] = []
    occupied: list[tuple[int, int]] = []
    remote_parity = seed & 1
    margin = SAMPLE_RATE // 2

    for index in range(target_count + negative_count):
        is_target = index < target_count
        if is_target:
            forced = ("gunshot", "footstep", "gunshot", "footstep")
            label = forced[index] if index < len(forced) else rng.choices(CLASS_NAMES, weights=(0.48, 0.52), k=1)[0]
            asset = rng.choice(by_label[label])
        else:
            label = "negative"
            asset = rng.choice(negatives)

        clip, onset_offset = _load_trimmed(root, asset)
        source_hint = "unknown" if not is_target else "self"
        render_mode = "self"
        spatial = SpatialParameters(0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0)
        if is_target and renderer is not None and index % 2 == remote_parity:
            source_hint = "remote"
            render_mode = "remote"
            spatial = _spatial_parameters(rng)
            clip = renderer.render(clip, spatial)
        elif is_target:
            # A self source is centered/symmetric in synthetic data.  The
            # source head must still learn an unknown state from real audio.
            mono = clip.mean(axis=1, keepdims=True)
            clip = np.repeat(mono, 2, axis=1)
        clip = _augment(clip, rng, complex_scene)
        trimmed, onset_offset = trim_activity_with_offset(Audio(clip, SAMPLE_RATE))
        clip = trimmed.samples
        if len(clip) >= total_frames - 2 * margin:
            clip = clip[:max(1, total_frames // 2)]
            onset_offset = min(onset_offset, len(clip) - 1)

        earliest = margin + onset_offset
        latest = total_frames - (len(clip) - onset_offset) - margin
        if latest <= earliest:
            raise ValueError("session is too short for an asset after trimming")
        desired_onset = rng.randint(earliest, latest)
        if is_target and index == 2:
            desired_onset = min(latest, events[0].onset_sample + 60 * SAMPLE_RATE // 1000)
        elif is_target and index == 3:
            desired_onset = min(latest, events[1].onset_sample + 100 * SAMPLE_RATE // 1000)
        elif is_target and complex_scene and index == 1 and events:
            desired_onset = min(latest, events[0].onset_sample + 20 * SAMPLE_RATE // 1000)
        elif is_target and not complex_scene:
            for _ in range(100):
                event_end = desired_onset + SAMPLE_RATE // 20
                if all(event_end + SAMPLE_RATE // 5 <= old_start or
                       desired_onset >= old_end + SAMPLE_RATE // 5 for old_start, old_end in occupied):
                    break
                desired_onset = rng.randint(earliest, latest)

        target_snr = (
            rng.uniform(10.0, 24.0) if is_target and not complex_scene
            else rng.uniform(-10.0, 20.0) if is_target
            else rng.uniform(-8.0, 12.0)
        )
        start, clip_end, achieved_snr = _place_clip(mix, clip, onset_offset, desired_onset, target_snr)
        event_end = min(clip_end, desired_onset + SAMPLE_RATE // 20)
        overlapping = [old_index for old_index, (old_start, old_end) in enumerate(occupied)
                       if desired_onset < old_end and event_end > old_start]
        for old_index in overlapping:
            events[old_index] = replace(events[old_index], overlap=True)
        occupied.append((desired_onset, event_end))
        events.append(TimelineEvent(
            event_id=f"{split}_{stratum}_{seed}_{index:05d}", sound_class=label,
            onset_sample=desired_onset, end_sample=event_end, source_asset=asset.relative_path,
            source_group=asset.source_group, stratum=stratum, snr_db=round(achieved_snr, 3),
            overlap=bool(overlapping), seen_source=split != "test", source_hint=source_hint,
            negative_subtype="" if is_target else (asset.subtype or asset.label),
            render_mode=render_mode, gain_db=round(session_gain_db, 3),
            distance_m=round(spatial.distance_meters, 3),
            azimuth_quadrant=spatial.azimuth_quadrant if render_mode == "remote" else "center",
            occluded=spatial.occlusion > 0.0, source_family=asset.source_group,
            scene_mode="busy" if complex_scene else "quiet",
        ))

    # Model common game/OS mastering after all sources have mixed.
    if complex_scene and rng.random() < 0.7:
        drive = rng.uniform(1.0, 2.0)
        mix = np.tanh(mix * drive) / np.tanh(drive)
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
    if not (0.0 <= ambient_only_fraction <= 1.0):
        raise ValueError("ambient_only_fraction must be between zero and one")
    if session_gain_min_db > session_gain_max_db:
        raise ValueError("session gain range is inverted")
    ambient_count = int(round(count * ambient_only_fraction))
    ambient_indices = set(random.Random(seed ^ 0x51A7E).sample(range(count), ambient_count))
    paths: list[Path] = []
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
            "generator_version": "mixture-v4", "seed": session_seed, "split": split,
            "stratum": stratum, "sample_rate": SAMPLE_RATE, "duration_seconds": duration_seconds,
            "ambient_only": ambient_only, "session_gain_db": round(session_gain_db, 3),
            "renderer": "steam-audio-v4.8.1" if renderer is not None else "self",
        })
        paths.append(prefix)
    return paths
