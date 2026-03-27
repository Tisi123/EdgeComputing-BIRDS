import argparse
import random
import shutil
from pathlib import Path

from tqdm import tqdm


def parse_args():
    parser = argparse.ArgumentParser(
        description="Merge negative image sources into one folder with per-source caps."
    )
    parser.add_argument(
        "--source",
        action="append",
        default=[],
        help="Source spec in format tag=path. Can be passed multiple times.",
    )
    parser.add_argument(
        "--cap",
        action="append",
        default=[],
        help="Optional cap spec in format tag=count. Can be passed multiple times.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("dataset/raw/neg_not_bird_all"),
        help="Destination folder.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for sampling.",
    )
    return parser.parse_args()


def parse_map(items, value_cast=str):
    out = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"Invalid spec (expected key=value): {item}")
        k, v = item.split("=", 1)
        k = k.strip()
        v = value_cast(v.strip())
        if not k:
            raise ValueError(f"Invalid key in spec: {item}")
        out[k] = v
    return out


def list_images(path: Path):
    exts = {".jpg", ".jpeg", ".png"}
    if not path.exists():
        return []
    return [p for p in path.iterdir() if p.is_file() and p.suffix.lower() in exts]


def main():
    args = parse_args()
    source_map = parse_map(args.source, str)
    if not source_map:
        raise ValueError("At least one --source tag=path is required.")
    cap_map = parse_map(args.cap, int) if args.cap else {}

    rnd = random.Random(args.seed)

    if args.out_dir.exists():
        shutil.rmtree(args.out_dir)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    summary = []
    total_saved = 0

    for tag, path_text in source_map.items():
        src_dir = Path(path_text)
        paths = list_images(src_dir)
        cap = cap_map.get(tag, 0)
        if cap > 0 and len(paths) > cap:
            paths = rnd.sample(paths, cap)

        saved = 0
        for i, p in enumerate(tqdm(sorted(paths), desc=f"Merging {tag}")):
            out_name = f"{tag}_{i:06d}{p.suffix.lower()}"
            shutil.copy(p, args.out_dir / out_name)
            saved += 1

        summary.append((tag, str(src_dir), len(list_images(src_dir)), cap if cap > 0 else "all", saved))
        total_saved += saved

    print("\nNegative merge summary")
    print("-" * 72)
    print(f"{'tag':12} {'source_dir':30} {'available':>10} {'cap':>10} {'saved':>8}")
    print("-" * 72)
    for tag, src, available, cap, saved in summary:
        src_short = src if len(src) <= 30 else "..." + src[-27:]
        print(f"{tag:12} {src_short:30} {available:10d} {str(cap):>10} {saved:8d}")
    print("-" * 72)
    print(f"Output dir: {args.out_dir}")
    print(f"Total saved: {total_saved}")


if __name__ == "__main__":
    main()
