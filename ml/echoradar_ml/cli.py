from __future__ import annotations

import argparse
from pathlib import Path

from .manifest import assert_no_leakage, grouped_split, read_inventory, write_split_manifest
from .mixtures import generate_from_manifest
from .training import train_and_export
from .evaluation import evaluate_sessions
from .inference import check_onnx_parity
from .annotation import review_timeline
from .spatial import SteamAudioRenderer


def _prefixes(directory: Path, pattern: str) -> list[Path]:
    return sorted(path.with_suffix("") for path in directory.glob(pattern) if path.suffix == ".wav")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="EchoRadar CS2 recognition pipeline")
    commands = parser.add_subparsers(dest="command", required=True)

    split = commands.add_parser("split", help="create a leakage-safe grouped split")
    split.add_argument("--inventory", required=True, type=Path)
    split.add_argument("--output", required=True, type=Path)
    split.add_argument("--seed", type=int, default=20260720)

    mixtures = commands.add_parser("mixtures", help="generate deterministic continuous sessions")
    mixtures.add_argument("--manifest", required=True, type=Path)
    mixtures.add_argument("--asset-root", required=True, type=Path)
    mixtures.add_argument("--output", required=True, type=Path)
    mixtures.add_argument("--split", choices=("train", "dev", "test"), required=True)
    mixtures.add_argument("--stratum", choices=("simple", "complex"), required=True)
    mixtures.add_argument("--count", type=int, default=10)
    mixtures.add_argument("--duration", type=float, default=60.0)
    mixtures.add_argument("--seed", type=int, default=20260720)
    mixtures.add_argument("--steam-audio-renderer", type=Path,
                          help="pinned v4.8.1 offline renderer; absent means self-like centered audio only")
    mixtures.add_argument("--ambient-only-fraction", type=float, default=0.25)
    mixtures.add_argument("--session-gain-min-db", type=float, default=-30.0)
    mixtures.add_argument("--session-gain-max-db", type=float, default=6.0)

    corpus = commands.add_parser(
        "corpus", help="generate the locked v3 160/40/40 spatial session corpus"
    )
    corpus.add_argument("--manifest", required=True, type=Path)
    corpus.add_argument("--asset-root", required=True, type=Path)
    corpus.add_argument("--output", required=True, type=Path)
    corpus.add_argument("--steam-audio-renderer", required=True, type=Path)
    corpus.add_argument("--duration", type=float, default=30.0)
    corpus.add_argument("--train-count", type=int, default=160)
    corpus.add_argument("--dev-count", type=int, default=40)
    corpus.add_argument("--test-count", type=int, default=40)
    corpus.add_argument("--seed", type=int, default=20260720)

    train = commands.add_parser("train", help="train and export the compact causal ONNX model")
    train.add_argument("--sessions", required=True, type=Path)
    train.add_argument("--output", required=True, type=Path)
    train.add_argument("--epochs", type=int, default=20)
    train.add_argument("--seed", type=int, default=20260720)

    evaluate = commands.add_parser("evaluate", help="run locked synthetic sessions through ONNX Runtime")
    evaluate.add_argument("--sessions", required=True, type=Path)
    evaluate.add_argument("--model", required=True, type=Path)
    evaluate.add_argument("--output", required=True, type=Path)
    evaluate.add_argument("--require-gates", action="store_true",
                          help="return failure unless every locked v3 synthetic gate passes")

    parity = commands.add_parser("parity", help="compare exported ONNX output with a PyTorch fixture")
    parity.add_argument("--model", required=True, type=Path)
    parity.add_argument("--fixture", required=True, type=Path)

    annotate = commands.add_parser("annotate", help="review a continuous WAV timeline")
    annotate.add_argument("--wav", required=True, type=Path)
    annotate.add_argument("--labels", required=True, type=Path)
    annotate.add_argument("--predictions", type=Path)

    args = parser.parse_args(argv)
    if args.command == "split":
        assets = grouped_split(read_inventory(args.inventory), args.seed)
        assert_no_leakage(assets)
        write_split_manifest(args.output, assets, args.seed)
        counts: dict[tuple[str, str], int] = {}
        for asset in assets:
            counts[(asset.split, asset.label)] = counts.get((asset.split, asset.label), 0) + 1
        for key in sorted(counts):
            print(f"{key[0]:5s} {key[1]:10s} {counts[key]:4d}")
        return 0
    if args.command == "mixtures":
        renderer = SteamAudioRenderer(args.steam_audio_renderer) if args.steam_audio_renderer else None
        paths = generate_from_manifest(
            args.manifest, args.asset_root, args.output, args.split, args.stratum,
            args.count, args.duration, args.seed, renderer=renderer,
            ambient_only_fraction=args.ambient_only_fraction,
            session_gain_min_db=args.session_gain_min_db,
            session_gain_max_db=args.session_gain_max_db,
        )
        print(f"generated {len(paths)} sessions in {args.output}")
        return 0
    if args.command == "corpus":
        renderer = SteamAudioRenderer(args.steam_audio_renderer)
        total = 0
        for split_index, (split_name, count) in enumerate((
            ("train", args.train_count), ("dev", args.dev_count), ("test", args.test_count)
        )):
            if count <= 0:
                parser.error("corpus split counts must be positive")
            simple_count = (count + 1) // 2
            for stratum_index, (stratum, stratum_count) in enumerate((
                ("simple", simple_count), ("complex", count - simple_count)
            )):
                if stratum_count == 0:
                    continue
                paths = generate_from_manifest(
                    args.manifest, args.asset_root, args.output, split_name, stratum,
                    stratum_count, args.duration,
                    args.seed + split_index * 100_000 + stratum_index * 10_000,
                    renderer=renderer, ambient_only_fraction=0.25,
                    session_gain_min_db=-30.0, session_gain_max_db=6.0,
                )
                total += len(paths)
        print(f"generated {total} locked v3 sessions in {args.output}")
        return 0
    if args.command == "train":
        train_prefixes = _prefixes(args.sessions, "train_*.wav")
        dev_prefixes = _prefixes(args.sessions, "dev_*.wav")
        if not train_prefixes or not dev_prefixes:
            parser.error("--sessions must contain train_*.wav and dev_*.wav")
        package = train_and_export(train_prefixes, dev_prefixes, args.output, args.epochs, args.seed)
        print(f"exported model package: {package}")
        return 0
    if args.command == "evaluate":
        report = evaluate_sessions(args.sessions, args.model, args.output)
        for stratum, values in report["strata"].items():
            print(stratum)
            for name in ("gunshot", "footstep", "mechanical"):
                metrics = values[name]
                print(f"  {name:10s} precision={metrics['precision']:.3f} recall={metrics['recall']:.3f} "
                      f"f1={metrics['f1']:.3f} fp/min={metrics['false_alerts_per_minute']:.2f}")
        print(f"v3 synthetic acceptance: {'PASS' if report['acceptance_passed'] else 'FAIL'}")
        return 1 if args.require_gates and not report["acceptance_passed"] else 0
    if args.command == "parity":
        error = check_onnx_parity(args.model, args.fixture)
        print(f"PyTorch/ONNX maximum absolute error: {error:.9g}")
        return 0
    if args.command == "annotate":
        review_timeline(args.wav, args.labels, args.predictions)
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
