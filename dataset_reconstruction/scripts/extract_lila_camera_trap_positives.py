import argparse
import csv
import json
import random
import shutil
from collections import Counter, defaultdict
from pathlib import Path

from PIL import Image
from tqdm import tqdm


DEFAULT_ENA24_BIRD_LABELS = [
    "bird",
    "crow",
    "corvid",
    "pigeon",
    "dove",
    "duck",
    "goose",
    "owl",
    "raptor",
]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Extract bird positives from COCO Camera Traps metadata."
    )
    parser.add_argument(
        "--metadata-json",
        type=Path,
        default=Path("dataset/external/lila/ena24/metadata.json"),
        help="Path to metadata JSON (COCO Camera Traps format).",
    )
    parser.add_argument(
        "--images-root",
        type=Path,
        default=Path("dataset/external/lila/ena24/images"),
        help="Root folder containing images referenced by metadata.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("dataset/raw/pos_bird_ena24"),
        help="Destination folder for extracted positives.",
    )
    parser.add_argument(
        "--dataset-tag",
        type=str,
        default="ena24",
        help="Filename prefix tag (example: ena24).",
    )
    parser.add_argument(
        "--target-label-mode",
        choices=["strict", "keyword"],
        default="strict",
        help="strict: exact label match, keyword: substring match.",
    )
    parser.add_argument(
        "--label-list",
        type=str,
        default=",".join(DEFAULT_ENA24_BIRD_LABELS),
        help="Comma-separated labels (strict) or keywords (keyword mode).",
    )
    parser.add_argument(
        "--max-images",
        type=int,
        default=0,
        help="Optional cap; <=0 means no cap.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for capped sampling.",
    )
    return parser.parse_args()


def normalize_list(text: str):
    return [s.strip().lower() for s in text.split(",") if s.strip()]


def resolve_image_path(images_root: Path, info: dict) -> Path:
    for key in ("file_name", "location", "relative_path", "path"):
        if key in info and info[key]:
            candidate = Path(info[key])
            if candidate.is_absolute():
                return candidate
            return images_root / candidate
    return images_root / f"{info.get('id', 'unknown')}.jpg"


def load_metadata(path: Path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    categories = {c["id"]: c["name"] for c in data.get("categories", [])}
    annotations_by_image = defaultdict(list)
    for ann in data.get("annotations", []):
        annotations_by_image[ann["image_id"]].append(ann)
    images = data.get("images", [])
    return images, categories, annotations_by_image


def is_bird(label: str, label_mode: str, targets: list[str]) -> bool:
    label_norm = label.lower().strip()
    if label_mode == "strict":
        return label_norm in targets
    return any(t in label_norm for t in targets)


def main():
    args = parse_args()

    if not args.metadata_json.exists():
        raise FileNotFoundError(f"Metadata not found: {args.metadata_json}")
    if not args.images_root.exists():
        raise FileNotFoundError(f"Images root not found: {args.images_root}")

    targets = normalize_list(args.label_list)
    if not targets:
        raise ValueError("--label-list cannot be empty")

    images, categories, anns_by_img = load_metadata(args.metadata_json)

    selected = []
    selected_labels = Counter()
    for info in images:
        image_id = info.get("id")
        anns = anns_by_img.get(image_id, [])
        cat_names = []
        keep = False
        for ann in anns:
            cat_name = categories.get(ann.get("category_id"), "").strip()
            if not cat_name:
                continue
            cat_names.append(cat_name)
            if is_bird(cat_name, args.target_label_mode, targets):
                keep = True
        if keep:
            selected.append((info, sorted(set(cat_names))))
            for label in set(cat_names):
                selected_labels[label] += 1

    if args.max_images and args.max_images > 0 and len(selected) > args.max_images:
        rnd = random.Random(args.seed)
        selected = rnd.sample(selected, args.max_images)

    if args.out_dir.exists():
        shutil.rmtree(args.out_dir)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    report_path = args.out_dir / "extraction_report.csv"
    saved = 0
    skipped_missing = 0
    skipped_load_error = 0

    rows = []
    for info, labels in tqdm(sorted(selected, key=lambda x: x[0].get("id", 0)), desc="Saving positives"):
        image_id = info.get("id")
        src = resolve_image_path(args.images_root, info)
        dst = args.out_dir / f"{args.dataset_tag}_{image_id}.jpg"

        if not src.exists():
            skipped_missing += 1
            rows.append((image_id, str(src), str(dst), "missing_source", "|".join(labels)))
            continue

        try:
            img = Image.open(src).convert("RGB")
            img.save(dst, quality=95)
            saved += 1
            rows.append((image_id, str(src), str(dst), "saved", "|".join(labels)))
        except Exception:
            skipped_load_error += 1
            rows.append((image_id, str(src), str(dst), "load_error", "|".join(labels)))

    with open(report_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["image_id", "src_path", "dst_path", "status", "labels"])
        writer.writerows(rows)

    print("\nPositive extraction summary")
    print("-" * 40)
    print(f"Metadata:            {args.metadata_json}")
    print(f"Images root:         {args.images_root}")
    print(f"Output dir:          {args.out_dir}")
    print(f"Target mode:         {args.target_label_mode}")
    print(f"Target labels:       {targets}")
    print(f"Candidate positives: {len(selected)}")
    print(f"Saved:               {saved}")
    print(f"Missing source:      {skipped_missing}")
    print(f"Load errors:         {skipped_load_error}")
    print(f"CSV report:          {report_path}")
    print("Category distribution among selected candidates:")
    for name, count in selected_labels.most_common(20):
        print(f"  {name}: {count}")


if __name__ == "__main__":
    main()
