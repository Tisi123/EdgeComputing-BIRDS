import argparse
from pathlib import Path

SPLITS = ["train", "val", "test"]
LABELS = ["bird", "not_bird"]
EXTS = {".jpg", ".jpeg", ".png"}


def parse_args():
    parser = argparse.ArgumentParser(description="Count images in a processed dataset split.")
    parser.add_argument("--base", type=Path, default=Path("dataset/bird_not_bird_96x96_reconstructed"))
    return parser.parse_args()


def count(folder: Path):
    if not folder.exists():
        return 0
    return sum(1 for f in folder.iterdir() if f.suffix.lower() in EXTS)


def main():
    args = parse_args()
    print(f"Base: {args.base}")
    print(f"{'':12} {'bird':>8} {'not_bird':>10} {'total':>8}")
    print("-" * 42)
    for split in SPLITS:
        counts = {label: count(args.base / split / label) for label in LABELS}
        total = sum(counts.values())
        print(f"{split:12} {counts['bird']:>8} {counts['not_bird']:>10} {total:>8}")


if __name__ == "__main__":
    main()
