import unittest

from echoradar_ml.manifest import Asset, assert_no_leakage, grouped_split


class ManifestTest(unittest.TestCase):
    def test_split_is_deterministic_and_group_safe(self):
        assets = []
        for label in ("gunshot", "footstep", "mechanical", "other"):
            for group in range(12):
                for item in range(2):
                    assets.append(Asset(
                        asset_id=f"{label}-{group}-{item}", relative_path=f"{label}/{group}/{item}.wav",
                        label=label, source_group=f"group-{group}", sha256=f"{label}-{group}-{item}",
                        duplicate_of="", included=True,
                    ))
        first = grouped_split(assets, 42)
        second = grouped_split(assets, 42)
        self.assertEqual(first, second)
        assert_no_leakage(first)
        self.assertEqual({asset.split for asset in first}, {"train", "dev", "test"})

    def test_duplicate_is_excluded_from_canonical_split(self):
        original = Asset("a", "a.wav", "gunshot", "g", "hash", "", True)
        duplicate = Asset("b", "b.wav", "gunshot", "g", "hash", "a.wav", True)
        result = grouped_split([original, duplicate])
        self.assertEqual([asset.asset_id for asset in result], ["a"])


if __name__ == "__main__":
    unittest.main()
