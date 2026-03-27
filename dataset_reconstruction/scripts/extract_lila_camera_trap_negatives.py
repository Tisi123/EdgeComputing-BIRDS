import argparse
import csv
import json
import random
import shutil
from collections import Counter, defaultdict
from pathlib import Path

from PIL import Image
from tqdm import tqdm


DEFAULT_BIRD_LABELS = [
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
        description="Extract negatives from COCO Camera Traps metadata."
    )
    parser.add_argument(
        "--metadata-json",
        type=Path,
        required=True,
        help="Path to metadata JSON (COCO Camera Traps format).",
    )
    parser.add_argument(
        "--images-root",
        type=Path,
        required=True,
        help="Root folder containing images referenced by metadata.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        required=True,
        help="Destination folder for extracted negatives.",
    )
    parser.add_argument(
        "--dataset-tag",
        type=str,
        default="lila",
        help="Filename prefix tag (example: ena24, cct20).",
    )
    parser.add_argument(
        "--negative-mode",
        choices=["empty_only", "non_bird_only", "empty_plus_non_bird"],
        default="empty_plus_non_bird",
        help="How to define negatives.",
    )
    parser.add_argument(
        "--bird-label-list",
        type=str,
        default=",".join(DEFAULT_BIRD_LABELS),
        help="Comma-separated list of labels treated as bird.",
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


def is_bird_label(label: str, bird_terms: list[str]) -> bool:
    norm = label.lower().strip()
    return any(term in norm for term in bird_terms)


def main():
    args = parse_args()

    if not args.metadata_json.exists():
        raise FileNotFoundError(f"Metadata not found: {args.metadata_json}")
    if not args.images_root.exists():
        raise FileNotFoundError(f"Images root not found: {args.images_root}")

    bird_terms = normalize_list(args.bird_label_list)
    if not bird_terms:
        raise ValueError("--bird-label-list cannot be empty")

    images, categories, anns_by_img = load_metadata(args.metadata_json)

    candidates = []
    mode_counter = Counter()

    for info in images:
        image_id = info.get("id")
        anns = anns_by_img.get(image_id, [])
        labels = []
        has_bird = False
        for ann in anns:
            label = categories.get(ann.get("category_id"), "").strip()
            if not label:
                continue
            labels.append(label)
            if is_bird_label(label, bird_terms):
                has_bird = True

        is_empty = len(anns) == 0
        is_non_bird_only = (len(anns) > 0) and (not has_bird)
        keep = False
        mode_tag = ""
        if args.negative_mode == "empty_only":
            keep = is_empty
            mode_tag = "empty"
        elif args.negative_mode == "non_bird_only":
            keep = is_non_bird_only
            mode_tag = "non_bird_only"
        else:
            keep = is_empty or is_non_bird_only
            mode_tag = "empty" if is_empty else "non_bird_only"

        if keep:
            candidates.append((info, sorted(set(labels)), mode_tag))
            mode_counter[mode_tag] += 1

    if args.max_images and args.max_images > 0 and len(candidates) > args.max_images:
        rnd = random.Random(args.seed)
        candidates = rnd.sample(candidates, args.max_images)

    if args.out_dir.exists():
        shutil.rmtree(args.out_dir)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    report_path = args.out_dir / "extraction_report.csv"

    saved = 0
    skipped_missing = 0
    skipped_load_error = 0
    rows = []

    for info, labels, mode_tag in tqdm(sorted(candidates, key=lambda x: x[0].get("id", 0)), desc="Saving negatives"):
        image_id = info.get("id")
        src = resolve_image_path(args.images_root, info)
        dst = args.out_dir / f"{args.dataset_tag}_{image_id}.jpg"
        if not src.exists():
            skipped_missing += 1
            rows.append((image_id, str(src), str(dst), "missing_source", mode_tag, "|".join(labels)))
            continue
        try:
            img = Image.open(src).convert("RGB")
            img.save(dst, quality=95)
            saved += 1
            rows.append((image_id, str(src), str(dst), "saved", mode_tag, "|".join(labels)))
        except Exception:
            skipped_load_error += 1
            rows.append((image_id, str(src), str(dst), "load_error", mode_tag, "|".join(labels)))

    with open(report_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["image_id", "src_path", "dst_path", "status", "neg_type", "labels"])
        writer.writerows(rows)

    print("\nNegative extraction summary")
    print("-" * 40)
    print(f"Metadata:            {args.metadata_json}")
    print(f"Images root:         {args.images_root}")
    print(f"Output dir:          {args.out_dir}")
    print(f"Negative mode:       {args.negative_mode}")
    print(f"Bird terms:          {bird_terms}")
    print(f"Candidate negatives: {len(candidates)}")
    print(f"Saved:               {saved}")
    print(f"Missing source:      {skipped_missing}")
    print(f"Load errors:         {skipped_load_error}")
    print(f"CSV report:          {report_path}")
    print("Composition:")
    for key in ["empty", "non_bird_only"]:
        print(f"  {key}: {mode_counter.get(key, 0)}")


if __name__ == "__main__":
    main()
