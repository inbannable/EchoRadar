from __future__ import annotations

import csv
from dataclasses import dataclass
import hashlib
from pathlib import Path
from typing import Iterable


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


def read_inventory(path: str | Path) -> list[Asset]:
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    return [
        Asset(
            asset_id=row["asset_id"],
            relative_path=row["relative_path"],
            label=row["label"],
            source_group=row["source_group"],
            sha256=row["sha256"],
            duplicate_of=row.get("duplicate_of", ""),
            included=row.get("included", "true").lower() == "true",
        )
        for row in rows
    ]


def _ordered_groups(groups: Iterable[str], seed: int, label: str) -> list[str]:
    def key(group: str) -> str:
        return hashlib.sha256(f"{seed}:{label}:{group}".encode()).hexdigest()

    return sorted(set(groups), key=key)


def grouped_split(assets: list[Asset], seed: int = 20260720) -> list[Asset]:
    canonical = [asset for asset in assets if asset.included and not asset.duplicate_of]
    result: list[Asset] = []
    for label in ("gunshot", "footstep", "mechanical", "other"):
        label_assets = [asset for asset in canonical if asset.label == label]
        groups: dict[str, list[Asset]] = {}
        for asset in label_assets:
            # Targets are held out by their meaningful family/surface group. Background
            # assets have no target subtype to generalize across, so they split per source
            # asset (duplicates were already removed above).
            grouping_value = asset.asset_id if label == "other" else (asset.source_group or asset.asset_id)
            key = f"{label}:{grouping_value}"
            groups.setdefault(key, []).append(asset)
        ordered = _ordered_groups(groups, seed, label)
        total = sum(len(groups[group]) for group in ordered)
        train_limit = total * 0.70
        dev_limit = total * 0.85
        assigned = 0
        for group in ordered:
            midpoint = assigned + len(groups[group]) / 2.0
            split = "train" if midpoint <= train_limit else "dev" if midpoint <= dev_limit else "test"
            for asset in groups[group]:
                result.append(Asset(**{**asset.__dict__, "split": split}))
            assigned += len(groups[group])
    return sorted(result, key=lambda asset: (asset.split, asset.label, asset.relative_path))


def write_split_manifest(path: str | Path, assets: list[Asset], seed: int) -> None:
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=[
            "split_version", "seed", "split", "asset_id", "relative_path", "label",
            "source_group", "sha256", "duplicate_of", "included",
        ])
        writer.writeheader()
        for asset in assets:
            writer.writerow({
                "split_version": "grouped-v1",
                "seed": seed,
                **asset.__dict__,
                "included": str(asset.included).lower(),
            })


def load_split_manifest(path: str | Path) -> list[Asset]:
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        return [
            Asset(
                asset_id=row["asset_id"], relative_path=row["relative_path"], label=row["label"],
                source_group=row["source_group"], sha256=row["sha256"],
                duplicate_of=row.get("duplicate_of", ""),
                included=row.get("included", "true").lower() == "true", split=row["split"],
            )
            for row in csv.DictReader(stream)
        ]


def assert_no_leakage(assets: list[Asset]) -> None:
    hashes: dict[str, str] = {}
    groups: dict[tuple[str, str], str] = {}
    for asset in assets:
        old_hash_split = hashes.setdefault(asset.sha256, asset.split)
        if old_hash_split != asset.split:
            raise ValueError(f"duplicate hash leaked across splits: {asset.sha256}")
        grouping_value = asset.asset_id if asset.label == "other" else (asset.source_group or asset.asset_id)
        group_key = (asset.label, grouping_value)
        old_group_split = groups.setdefault(group_key, asset.split)
        if old_group_split != asset.split:
            raise ValueError(f"source group leaked across splits: {group_key}")
