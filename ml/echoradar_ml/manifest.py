from __future__ import annotations

import csv
from dataclasses import dataclass, replace
import hashlib
from pathlib import Path
from typing import Iterable

from . import CLASS_NAMES
from .audio import load_pcm_wav, to_stereo_48k, trim_activity_with_offset


NEGATIVE_LABELS = ("mechanical", "other", "negative")

FIREARM_FOLDERS = {
    "ak47", "aug", "awp", "bizon", "cz75a", "deagle", "elite", "famas", "fiveseven",
    "g3sg1", "galilar", "glock18", "hkp2000", "m249", "m4a1", "mac10", "mag7", "mp5",
    "mp7", "mp9", "negev", "nova", "p250", "p90", "revolver", "sawedoff", "scar20",
    "sg556", "ssg08", "tec9", "ump45", "usp", "xm1014",
}
MECHANICAL_TOKEN_GROUPS = (
    (("clip", "reload", "box", "chain", "cover", "insertshell"), "reload"),
    (("bolt", "slide", "pump", "hammer", "prepare", "sideback", "siderelease", "mech"),
     "weapon_action"),
    (("silencer_on", "silencer_off", "silencer_screw"), "attachment"),
    (("draw", "deploy"), "draw"),
    (("zoom",), "zoom"),
    (("switch",), "switch"),
    (("pinpull", "throw"), "grenade_handling"),
    (("plant", "disarm", "key_press", "initiate", "click"), "device_handling"),
    (("movement", "lowammo", "special", "taunt", "element"), "handling"),
)


@dataclass(frozen=True)
class Asset:
    asset_id: str
    relative_path: str
    label: str
    source_group: str
    sha256: str
    duplicate_of: str
    included: bool
    split: str = ""
    subtype: str = ""
    review_status: str = "auto"
    active_offset_samples: int = -1

    @property
    def is_target(self) -> bool:
        return self.label in CLASS_NAMES

    @property
    def training_label(self) -> str:
        return self.label if self.is_target else "negative"


def _as_bool(value: str | bool | None, fallback: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if value is None or value == "":
        return fallback
    return value.lower() in ("1", "true", "yes", "y")


def read_inventory(path: str | Path) -> list[Asset]:
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    assets: list[Asset] = []
    for row in rows:
        label = row.get("event_class") or row.get("label") or "other"
        if label not in (*CLASS_NAMES, *NEGATIVE_LABELS):
            label = "other"
        active_offset = row.get("active_offset_samples", "")
        review_status = row.get("review_status", "")
        if not review_status:
            try:
                review_status = "review" if float(row.get("classification_confidence", 1.0)) < 0.80 else "accepted"
            except ValueError:
                review_status = "review"
        assets.append(Asset(
            asset_id=row["asset_id"],
            relative_path=row["relative_path"],
            label=label,
            source_group=row.get("source_group", ""),
            sha256=row.get("sha256", ""),
            duplicate_of=row.get("duplicate_of", ""),
            included=_as_bool(row.get("included"), True),
            split=row.get("split", ""),
            subtype=row.get("negative_subtype") or row.get("subtype", ""),
            review_status=review_status,
            active_offset_samples=int(active_offset) if active_offset not in (None, "") else -1,
        ))
    return assets


def _ordered_groups(groups: Iterable[str], seed: int, label: str) -> list[str]:
    def key(group: str) -> str:
        return hashlib.sha256(f"{seed}:{label}:{group}".encode()).hexdigest()

    return sorted(set(groups), key=key)


def grouped_split(assets: list[Asset], seed: int = 20260720) -> list[Asset]:
    """Split canonical assets without leaking weapon/surface families.

    All non-emitted sounds share the training label ``negative`` but split per
    canonical file; this maximizes negative diversity without pretending that
    broad mechanical folders are coherent acoustic classes.
    """
    canonical = [asset for asset in assets if asset.included and not asset.duplicate_of]
    result: list[Asset] = []
    labels = (*CLASS_NAMES, "negative")
    for training_label in labels:
        label_assets = [asset for asset in canonical if asset.training_label == training_label]
        groups: dict[str, list[Asset]] = {}
        for asset in label_assets:
            grouping_value = (
                asset.source_group or asset.asset_id
                if training_label in CLASS_NAMES else asset.asset_id
            )
            key = f"{training_label}:{grouping_value}"
            groups.setdefault(key, []).append(asset)
        ordered = _ordered_groups(groups, seed, training_label)
        total = sum(len(groups[group]) for group in ordered)
        train_limit = total * 0.70
        dev_limit = total * 0.85
        assigned = 0
        for group in ordered:
            midpoint = assigned + len(groups[group]) / 2.0
            split = "train" if midpoint <= train_limit else "dev" if midpoint <= dev_limit else "test"
            result.extend(replace(asset, split=split) for asset in groups[group])
            assigned += len(groups[group])
    return sorted(result, key=lambda asset: (asset.split, asset.training_label, asset.relative_path))


MANIFEST_FIELDS = [
    "split_version", "seed", "split", "asset_id", "relative_path", "event_class",
    "negative_subtype", "source_group", "sha256", "duplicate_of", "included",
    "review_status", "active_offset_samples",
]


def write_split_manifest(path: str | Path, assets: list[Asset], seed: int) -> None:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=MANIFEST_FIELDS)
        writer.writeheader()
        for asset in assets:
            writer.writerow({
                "split_version": "grouped-v4",
                "seed": seed,
                "split": asset.split,
                "asset_id": asset.asset_id,
                "relative_path": asset.relative_path,
                "event_class": asset.label if asset.is_target else "negative",
                "negative_subtype": "" if asset.is_target else (asset.subtype or asset.label),
                "source_group": asset.source_group,
                "sha256": asset.sha256,
                "duplicate_of": asset.duplicate_of,
                "included": str(asset.included).lower(),
                "review_status": asset.review_status,
                "active_offset_samples": asset.active_offset_samples,
            })


def load_split_manifest(path: str | Path) -> list[Asset]:
    return read_inventory(path)


def assert_no_leakage(assets: list[Asset]) -> None:
    hashes: dict[str, str] = {}
    groups: dict[tuple[str, str], str] = {}
    for asset in assets:
        if not asset.split:
            raise ValueError(f"asset has no split: {asset.relative_path}")
        if asset.sha256:
            old_hash_split = hashes.setdefault(asset.sha256, asset.split)
            if old_hash_split != asset.split:
                raise ValueError(f"duplicate hash leaked across splits: {asset.sha256}")
        grouping_value = (asset.source_group or asset.asset_id) if asset.is_target else asset.asset_id
        group_key = (asset.training_label, grouping_value)
        old_group_split = groups.setdefault(group_key, asset.split)
        if old_group_split != asset.split:
            raise ValueError(f"source group leaked across splits: {group_key}")


def prepare_onset_offsets(assets: list[Asset], asset_root: str | Path) -> list[Asset]:
    """Measure audible onsets in normalized 48 kHz samples.

    Invalid rows are retained for review but excluded from training.  Exact
    duplicates keep their own row and later inherit no split.
    """
    root = Path(asset_root)
    prepared: list[Asset] = []
    for asset in assets:
        try:
            audio = to_stereo_48k(load_pcm_wav(root / asset.relative_path))
            _, offset = trim_activity_with_offset(audio)
            prepared.append(replace(asset, active_offset_samples=offset))
        except (OSError, ValueError):
            prepared.append(replace(asset, included=False, review_status="invalid"))
    return prepared


def _classify_relative_path(relative: Path) -> tuple[str, str, str, str]:
    parts = [part.lower() for part in relative.parts]
    stem = relative.stem.lower()
    if len(parts) >= 3 and parts[0] == "player" and parts[1] == "footsteps":
        surface = stem.rsplit("_", 1)[0] if stem.rsplit("_", 1)[-1].isdigit() else stem
        if surface.endswith("_ct"):
            surface = surface[:-3]
        elif surface.endswith("_t"):
            surface = surface[:-2]
        subtype = "landing" if surface.startswith("land_") else "step"
        return "footstep", subtype, f"surface:{surface}", "accepted"

    mechanical = next(
        (subtype for tokens, subtype in MECHANICAL_TOKEN_GROUPS if any(token in stem for token in tokens)),
        "",
    )
    if len(parts) >= 3 and parts[0] == "weapons" and parts[1] in FIREARM_FOLDERS:
        if mechanical:
            return "mechanical", mechanical, f"weapon:{parts[1]}", "accepted"
        review = "review" if "clean" in stem else "auto"
        if review == "auto":
            review = "accepted"
        return "gunshot", "weapon_report", f"weapon:{parts[1]}", review
    if mechanical:
        group = f"weapon:{parts[1]}" if len(parts) >= 2 and parts[0] == "weapons" else "mechanical:misc"
        return "mechanical", mechanical, group, "accepted"
    if parts and parts[0] == "player":
        return "other", "player", "other:player", "accepted"
    if len(parts) >= 2 and parts[0] == "weapons" and parts[1] == "fx":
        subtype = "projectile_fx"
        group = f"other:weapon_fx/{parts[2]}" if len(parts) >= 3 else "other:weapon_fx"
        return "other", subtype, group, "accepted"
    if len(parts) >= 2 and parts[0] == "weapons":
        subtype = "melee" if parts[1].startswith("knife") or parts[1] == "bknife" else "weapon_other"
        return "other", subtype, f"other:weapon/{parts[1]}", "accepted"
    return "other", "other", f"other:{parts[0] if parts else 'misc'}", "accepted"


def scan_asset_tree(asset_root: str | Path) -> list[Asset]:
    """Create a reviewable manifest directly from a local CS2 WAV tree."""
    root = Path(asset_root)
    if not root.is_dir():
        raise ValueError(f"asset root is not a directory: {root}")
    records: list[Asset] = []
    canonical_by_hash: dict[str, str] = {}
    for path in sorted(root.rglob("*.wav")):
        if any(part == ".DS_Store" or part.startswith("._") for part in path.relative_to(root).parts):
            continue
        relative = path.relative_to(root)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        duplicate = canonical_by_hash.setdefault(digest, relative.as_posix())
        if duplicate == relative.as_posix():
            duplicate = ""
        label, subtype, group, review = _classify_relative_path(relative)
        included = True
        offset = -1
        try:
            audio = to_stereo_48k(load_pcm_wav(path))
            _, offset = trim_activity_with_offset(audio)
        except (OSError, ValueError):
            included = False
            review = "invalid"
        records.append(Asset(
            asset_id=f"asset_{digest[:16]}", relative_path=relative.as_posix(), label=label,
            source_group=group, sha256=digest, duplicate_of=duplicate, included=included,
            subtype=subtype, review_status=review, active_offset_samples=offset,
        ))
    return records


def manifest_summary(assets: list[Asset]) -> dict[str, int]:
    summary = {"total": len(assets), "included": 0, "duplicates": 0, "needs_review": 0}
    for name in (*CLASS_NAMES, "negative"):
        summary[name] = 0
    for asset in assets:
        summary[asset.training_label] += 1
        summary["included"] += int(asset.included)
        summary["duplicates"] += int(bool(asset.duplicate_of))
        summary["needs_review"] += int(asset.review_status not in ("reviewed", "accepted"))
    return summary
