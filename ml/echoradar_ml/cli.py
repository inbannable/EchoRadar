from __future__ import annotations

import argparse
from dataclasses import asdict
import json
from pathlib import Path

from .annotation import review_timeline
from .direction import (
    DirectionCorpusRow,
    DirectionFeatures,
    FULL_DIRECTION_COUNTS,
    SMOKE_DIRECTION_COUNTS,
    check_direction_parity,
    evaluate_direction_rows,
    evaluate_direction_package,
    generate_direction_corpus,
    generate_direction_mixtures,
    prepare_direction_cache,
    train_direction_model,
)
from .evaluation import evaluate_sessions
from .inference import check_onnx_parity
from .manifest import (
    assert_no_leakage, grouped_split, manifest_summary, prepare_onset_offsets,
    load_split_manifest, read_inventory, scan_asset_tree, write_split_manifest,
)
from .mixtures import generate_from_manifest
from .sessions import audit_session_corpus, import_real_session, training_readiness
from .spatial import SteamAudioRenderer
from .training import prepare_feature_cache, train_and_export


def _prefixes(directory: Path, pattern: str) -> list[Path]:
    return sorted(path.with_suffix("") for path in directory.glob(pattern) if path.suffix == ".wav")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="EchoRadar v4 CPU sound-onset training pipeline")
    commands = parser.add_subparsers(dest="command", required=True)

    prepare = commands.add_parser(
        "prepare-assets", help="scan, onset-align, group-split, and write a reviewable v4 asset manifest"
    )
    prepare.add_argument("--asset-root", required=True, type=Path)
    prepare.add_argument("--output", required=True, type=Path)
    prepare.add_argument("--seed", type=int, default=20260720)

    split = commands.add_parser("split", help="convert an existing inventory to the grouped v4 split")
    split.add_argument("--inventory", required=True, type=Path)
    split.add_argument("--asset-root", type=Path,
                       help="measure exact 48 kHz onset offsets; strongly recommended")
    split.add_argument("--output", required=True, type=Path)
    split.add_argument("--seed", type=int, default=20260720)

    mixtures = commands.add_parser("mixtures", help="generate deterministic v4 continuous sessions")
    mixtures.add_argument("--manifest", required=True, type=Path)
    mixtures.add_argument("--asset-root", required=True, type=Path)
    mixtures.add_argument("--output", required=True, type=Path)
    mixtures.add_argument("--split", choices=("train", "dev", "test"), required=True)
    mixtures.add_argument("--stratum", choices=("simple", "complex"), required=True)
    mixtures.add_argument("--count", type=int, default=10)
    mixtures.add_argument("--duration", type=float, default=30.0)
    mixtures.add_argument("--seed", type=int, default=20260720)
    mixtures.add_argument("--steam-audio-renderer", type=Path)
    mixtures.add_argument("--ambient-only-fraction", type=float, default=0.25)
    mixtures.add_argument("--session-gain-min-db", type=float, default=-30.0)
    mixtures.add_argument("--session-gain-max-db", type=float, default=6.0)

    corpus = commands.add_parser("corpus", help="generate the locked 160/40/40 v4 spatial corpus")
    corpus.add_argument("--manifest", required=True, type=Path)
    corpus.add_argument("--asset-root", required=True, type=Path)
    corpus.add_argument("--output", required=True, type=Path)
    corpus.add_argument("--steam-audio-renderer", required=True, type=Path)
    corpus.add_argument("--duration", type=float, default=30.0)
    corpus.add_argument("--train-count", type=int, default=160)
    corpus.add_argument("--dev-count", type=int, default=40)
    corpus.add_argument("--test-count", type=int, default=40)
    corpus.add_argument("--seed", type=int, default=20260720)

    real = commands.add_parser("import-real", help="normalize one reviewed gameplay recording")
    real.add_argument("--wav", required=True, type=Path)
    real.add_argument("--labels", required=True, type=Path)
    real.add_argument("--output", required=True, type=Path)
    real.add_argument("--split", choices=("train", "dev", "test"), required=True)
    real.add_argument("--session-id", required=True)
    real.add_argument("--map", required=True, dest="map_name")
    real.add_argument("--capture-day", required=True)
    real.add_argument("--audio-settings", required=True)
    real.add_argument("--label-sample-rate", type=int, default=48000,
                      help="sample coordinates used by JSONL labels; annotate writes 48000")

    audit = commands.add_parser("audit", help="report corpus support and locked-data readiness")
    audit.add_argument("--sessions", required=True, type=Path)
    audit.add_argument("--output", type=Path)
    audit.add_argument("--require-locked", action="store_true")

    cache = commands.add_parser("cache", help="precompute disk-backed v4 features before CPU training")
    cache.add_argument("--sessions", required=True, type=Path)
    cache.add_argument("--output", required=True, type=Path)

    train = commands.add_parser("train", help="train/export the compact v4 model on CPU")
    train.add_argument("--sessions", required=True, type=Path)
    train.add_argument("--output", required=True, type=Path)
    train.add_argument("--cache", type=Path)
    train.add_argument("--epochs", type=int, default=20)
    train.add_argument("--batch-size", type=int, default=64)
    train.add_argument("--threads", type=int, default=0,
                       help="PyTorch CPU threads; zero keeps its platform default")
    train.add_argument("--resume", type=Path)
    train.add_argument("--seed", type=int, default=20260720)

    evaluate = commands.add_parser("evaluate", help="run locked sessions through ONNX Runtime")
    evaluate.add_argument("--sessions", required=True, type=Path)
    evaluate.add_argument("--model", required=True, type=Path)
    evaluate.add_argument("--output", required=True, type=Path)
    evaluate.add_argument("--require-gates", action="store_true")

    parity = commands.add_parser("parity", help="compare exported ONNX and PyTorch outputs")
    parity.add_argument("--model", required=True, type=Path)
    parity.add_argument("--fixture", required=True, type=Path)

    annotate = commands.add_parser("annotate", help="review a continuous WAV timeline")
    annotate.add_argument("--wav", required=True, type=Path)
    annotate.add_argument("--labels", required=True, type=Path)
    annotate.add_argument("--predictions", type=Path)

    direction = commands.add_parser(
        "direction-corpus", help="render a deterministic known-bearing Steam Audio corpus"
    )
    direction.add_argument("--source-root", required=True, type=Path)
    direction.add_argument("--output", required=True, type=Path)
    direction.add_argument("--steam-audio-renderer", required=True, type=Path)
    direction.add_argument("--azimuth-step", type=int, default=15)
    direction.add_argument("--max-sources", type=int, default=0)
    direction.add_argument(
        "--conditions", nargs="+",
        default=("clean", "gain", "noise", "mild-reverb", "occlusion", "channel-isolation"),
        choices=("clean", "gain", "noise", "mild-reverb", "occlusion", "channel-isolation"),
    )

    direction_mixtures = commands.add_parser(
        "direction-mixtures",
        help="render deterministic 0-3 source 3D direction scenes through Steam Audio",
    )
    direction_mixtures.add_argument("--manifest", required=True, type=Path)
    direction_mixtures.add_argument("--asset-root", required=True, type=Path)
    direction_mixtures.add_argument("--output", required=True, type=Path)
    direction_mixtures.add_argument("--steam-audio-renderer", required=True, type=Path)
    direction_mixtures.add_argument("--preset", choices=("full", "smoke"), default="full")
    direction_mixtures.add_argument("--train-count", type=int)
    direction_mixtures.add_argument("--dev-count", type=int)
    direction_mixtures.add_argument("--test-count", type=int)
    direction_mixtures.add_argument("--close-test-fraction", type=float, default=0.20)
    direction_mixtures.add_argument("--seed", type=int, default=20260720)

    direction_cache = commands.add_parser(
        "direction-cache", help="cache [5,48,64] features and Multi-ACCDOA targets"
    )
    direction_cache.add_argument("--manifest", required=True, type=Path)
    direction_cache.add_argument("--audio-root", type=Path)
    direction_cache.add_argument("--output", required=True, type=Path)
    direction_cache.add_argument(
        "--splits", nargs="+", choices=("train", "dev", "test"),
        default=("train", "dev", "test"),
    )
    direction_cache.add_argument("--feature-dtype", choices=("float16", "float32"),
                                 default="float16")

    direction_train = commands.add_parser(
        "direction-train", help="train and package the compact 3D Multi-ACCDOA model"
    )
    direction_train.add_argument("--cache", required=True, type=Path)
    direction_train.add_argument("--output", required=True, type=Path)
    direction_train.add_argument("--epochs", type=int, default=20)
    direction_train.add_argument("--batch-size", type=int, default=64)
    direction_train.add_argument("--threads", type=int, default=0)
    direction_train.add_argument("--resume", type=Path)
    direction_train.add_argument("--seed", type=int, default=20260720)

    direction_parity = commands.add_parser(
        "direction-parity", help="compare direction.onnx against its PyTorch parity fixture"
    )
    direction_parity.add_argument("--model", required=True, type=Path,
                                  help="direction model package directory")
    direction_parity.add_argument("--fixture", type=Path)

    direction_evaluate = commands.add_parser(
        "direction-evaluate",
        help="evaluate a multi-source package or the legacy single-source v2 mapper",
    )
    direction_evaluate.add_argument("--manifest", required=True, type=Path)
    direction_evaluate.add_argument("--model", type=Path,
                                    help="direction package; omit for the legacy v2 mapper")
    direction_evaluate.add_argument("--audio-root", type=Path)
    direction_evaluate.add_argument("--output", type=Path)
    direction_evaluate.add_argument("--error-clips", type=Path)
    direction_evaluate.add_argument("--split", choices=("all", "train", "validation", "dev", "test"),
                                    default="all")
    direction_evaluate.add_argument("--footstep-only", action="store_true")
    direction_evaluate.add_argument("--gate", choices=("auto", "synthetic", "real"),
                                    default="auto")
    direction_evaluate.add_argument("--require-gates", action="store_true")

    args = parser.parse_args(argv)
    if args.command == "prepare-assets":
        assets = scan_asset_tree(args.asset_root)
        split_assets = grouped_split(assets, args.seed)
        assert_no_leakage(split_assets)
        write_split_manifest(args.output, split_assets, args.seed)
        write_split_manifest(
            args.output.with_suffix(".review.csv"),
            [asset for asset in split_assets if asset.review_status == "review"], args.seed,
        )
        summary = manifest_summary(assets)
        args.output.with_suffix(".summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    if args.command == "split":
        assets = read_inventory(args.inventory)
        if args.asset_root:
            assets = prepare_onset_offsets(assets, args.asset_root)
        assets = grouped_split(assets, args.seed)
        assert_no_leakage(assets)
        write_split_manifest(args.output, assets, args.seed)
        write_split_manifest(
            args.output.with_suffix(".review.csv"),
            [asset for asset in assets if asset.review_status == "review"], args.seed,
        )
        print(json.dumps(manifest_summary(assets), indent=2, sort_keys=True))
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
        unresolved = [
            asset.relative_path for asset in load_split_manifest(args.manifest)
            if asset.review_status == "review"
        ]
        if unresolved:
            print("WARNING: unresolved asset review rows will be included:")
            for path in unresolved:
                print(f"  {path}")
        renderer = SteamAudioRenderer(args.steam_audio_renderer)
        total = 0
        for split_index, (split_name, count) in enumerate((
            ("train", args.train_count), ("dev", args.dev_count), ("test", args.test_count),
        )):
            if count <= 0:
                parser.error("corpus split counts must be positive")
            simple_count = (count + 1) // 2
            for stratum_index, (stratum, stratum_count) in enumerate((
                ("simple", simple_count), ("complex", count - simple_count),
            )):
                if not stratum_count:
                    continue
                total += len(generate_from_manifest(
                    args.manifest, args.asset_root, args.output, split_name, stratum,
                    stratum_count, args.duration,
                    args.seed + split_index * 100_000 + stratum_index * 10_000,
                    renderer=renderer, ambient_only_fraction=0.25,
                    session_gain_min_db=-30.0, session_gain_max_db=6.0,
                ))
        print(f"generated {total} locked v4 sessions in {args.output}")
        return 0
    if args.command == "import-real":
        prefix = import_real_session(
            args.wav, args.labels, args.output, args.split, args.session_id,
            args.map_name, args.capture_day, args.audio_settings, args.label_sample_rate,
        )
        print(f"imported {prefix}")
        return 0
    if args.command == "audit":
        report = audit_session_corpus(args.sessions)
        readiness = training_readiness(report)
        result = {**report, "readiness": readiness, "locked_ready": all(readiness.values())}
        text = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8")
        print(text, end="")
        return 1 if args.require_locked and not result["locked_ready"] else 0
    if args.command == "cache":
        train_prefixes = _prefixes(args.sessions, "train_*.wav")
        dev_prefixes = _prefixes(args.sessions, "dev_*.wav")
        if not train_prefixes or not dev_prefixes:
            parser.error("--sessions must contain train_*.wav and dev_*.wav")
        prepare_feature_cache(train_prefixes, args.output / "train", (-24.0, -12.0, 0.0))
        prepare_feature_cache(dev_prefixes, args.output / "dev", (0.0,))
        print(f"feature cache ready: {args.output}")
        return 0
    if args.command == "train":
        train_prefixes = _prefixes(args.sessions, "train_*.wav")
        dev_prefixes = _prefixes(args.sessions, "dev_*.wav")
        if not train_prefixes or not dev_prefixes:
            parser.error("--sessions must contain train_*.wav and dev_*.wav")
        package = train_and_export(
            train_prefixes, dev_prefixes, args.output, args.epochs, args.seed,
            cache_dir=args.cache, threads=args.threads, batch_size=args.batch_size,
            resume=args.resume,
        )
        print(f"exported model package: {package}")
        return 0
    if args.command == "evaluate":
        report = evaluate_sessions(args.sessions, args.model, args.output)
        for name, metrics in report["overall"].items():
            print(f"{name:10s} precision={metrics['precision']:.3f} recall={metrics['recall']:.3f} "
                  f"fp/min={metrics['false_alerts_per_minute']:.2f} "
                  f"p95_latency={metrics['p95_delivery_latency_ms']:.1f}ms")
        print(f"v4 acceptance: {'PASS' if report['acceptance_passed'] else 'FAIL'}")
        return 1 if args.require_gates and not report["acceptance_passed"] else 0
    if args.command == "parity":
        error = check_onnx_parity(args.model, args.fixture)
        print(f"PyTorch/ONNX maximum absolute error: {error:.9g}")
        return 0
    if args.command == "annotate":
        review_timeline(args.wav, args.labels, args.predictions)
        return 0
    if args.command == "direction-corpus":
        if args.azimuth_step <= 0 or 360 % args.azimuth_step != 0:
            parser.error("--azimuth-step must be a positive divisor of 360")
        sources = sorted(args.source_root.rglob("*.wav"))
        if args.max_sources > 0:
            sources = sources[:args.max_sources]
        if not sources:
            parser.error("--source-root contains no WAV files")
        renderer = SteamAudioRenderer(args.steam_audio_renderer)
        rendered = generate_direction_corpus(
            sources, args.output, renderer,
            tuple(float(angle) for angle in range(0, 360, args.azimuth_step)),
            conditions=args.conditions,
        )
        print(f"generated {len(rendered)} known-bearing clips in {args.output}")
        return 0
    if args.command == "direction-mixtures":
        counts = dict(FULL_DIRECTION_COUNTS if args.preset == "full" else SMOKE_DIRECTION_COUNTS)
        for split_name in ("train", "dev", "test"):
            override = getattr(args, f"{split_name}_count")
            if override is not None:
                if override <= 0:
                    parser.error(f"--{split_name}-count must be positive")
                counts[split_name] = override
        renderer = SteamAudioRenderer(args.steam_audio_renderer)
        manifest = generate_direction_mixtures(
            args.manifest, args.asset_root, args.output, renderer, counts,
            args.seed, args.close_test_fraction,
        )
        print(f"generated {sum(counts.values())} multi-source direction scenes: {manifest}")
        return 0
    if args.command == "direction-cache":
        cache = prepare_direction_cache(
            args.manifest, args.audio_root or args.manifest.parent, args.output,
            args.splits, args.feature_dtype,
        )
        print(f"direction feature cache ready: {cache}")
        return 0
    if args.command == "direction-train":
        package = train_direction_model(
            args.cache, args.output, args.epochs, args.seed, args.batch_size,
            args.threads, args.resume,
        )
        print(f"exported direction model package: {package}")
        return 0
    if args.command == "direction-parity":
        error = check_direction_parity(args.model, args.fixture)
        print(f"Direction PyTorch/ONNX maximum absolute error: {error:.9g}")
        return 0
    if args.command == "direction-evaluate":
        if args.model:
            if args.split not in ("train", "dev", "test"):
                parser.error("multi-source direction evaluation requires --split train, dev, or test")
            report = asdict(evaluate_direction_package(
                args.manifest, args.audio_root or args.manifest.parent,
                args.model, args.split, args.footstep_only, args.gate,
            ))
            text = json.dumps(report, indent=2, sort_keys=True)
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(text + "\n", encoding="utf-8")
            print(text)
            return 1 if args.require_gates and not report["acceptance_passed"] else 0
        if args.footstep_only or args.require_gates or args.audio_root or args.gate != "auto":
            parser.error("--footstep-only, --require-gates, --audio-root, and --gate require --model")
        if args.split in ("dev", "test"):
            parser.error("legacy direction evaluation supports --split all, train, or validation")
        rows = []
        for line in args.manifest.read_text(encoding="utf-8").splitlines():
            payload = json.loads(line)
            payload["features"] = DirectionFeatures(**payload["features"])
            row = DirectionCorpusRow(**payload)
            if args.split == "all" or row.source_split == args.split:
                rows.append(row)
        report = asdict(evaluate_direction_rows(
            rows,
            audio_root=args.manifest.parent,
            error_clip_directory=args.error_clips,
        ))
        text = json.dumps(report, indent=2, sort_keys=True)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text + "\n", encoding="utf-8")
        print(text)
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
