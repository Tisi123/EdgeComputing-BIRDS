import argparse
import csv
import re
import shutil
from collections import defaultdict
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(
        description="Import labeled ESP32 camera images into raw V5 source folders."
    )
    parser.add_argument(
        "--camera-root",
        type=Path,
        default=Path("own_images"),
        help="Root folder containing positives/, negatives/, and BIRDS.TXT.",
    )
    parser.add_argument(
        "--pos-out",
        type=Path,
        default=Path("dataset/raw/pos_bird_esp32"),
        help="Destination folder for ESP32 positive images.",
    )
    parser.add_argument(
        "--neg-out",
        type=Path,
        default=Path("dataset/raw/neg_not_bird_esp32"),
        help="Destination folder for ESP32 negative images.",
    )
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=Path("dataset/raw/esp32_camera_manifest.csv"),
        help="CSV manifest tying filenames to label/date/confidence.",
    )
    return parser.parse_args()


def reset_dir(path: Path):
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def list_images(path: Path):
    exts = {".jpg", ".jpeg", ".png"}
    if not path.exists():
        return []
    return sorted([p for p in path.iterdir() if p.is_file() and p.suffix.lower() in exts])


def parse_metadata(metadata_path: Path):
    text = metadata_path.read_text(encoding="utf-8", errors="replace").splitlines()
    pat_img = re.compile(r'"image":\s*"([^"]+\.(?:jpg|jpeg|png))', re.IGNORECASE)
    pat_dt = re.compile(r'"datetime":\s*"([^"]+)"')
    pat_conf = re.compile(r'"confidence":\s*([0-9.]+)')
    pat_ts = re.compile(r'"timestamp":\s*(\d+)')

    by_image = defaultdict(list)
    for line in text:
        if "image" not in line.lower():
            continue
        img_match = pat_img.search(line)
        if not img_match:
            continue
        dt_match = pat_dt.search(line)
        conf_match = pat_conf.search(line)
        ts_match = pat_ts.search(line)
        image_name = Path(img_match.group(1)).name.lower()
        by_image[image_name].append(
            {
                "datetime": dt_match.group(1) if dt_match else "",
                "date": dt_match.group(1).split("T", 1)[0] if dt_match else "",
                "confidence": float(conf_match.group(1)) if conf_match else 0.0,
                "timestamp": int(ts_match.group(1)) if ts_match else "",
            }
        )
    return by_image


def summarize_metadata(entries):
    if not entries:
        return {"date": "", "datetime": "", "confidence": "", "timestamp": ""}
    entries_sorted = sorted(entries, key=lambda x: (x["datetime"], x["confidence"]))
    best = max(entries, key=lambda x: x["confidence"])
    first = entries_sorted[0]
    return {
        "date": first["date"],
        "datetime": first["datetime"],
        "confidence": best["confidence"],
        "timestamp": best["timestamp"],
    }


def copy_labeled_images(src_dir: Path, dst_dir: Path):
    copied = []
    for path in list_images(src_dir):
        out_name = path.name.lower()
        shutil.copy(path, dst_dir / out_name)
        copied.append(out_name)
    return copied


def main():
    args = parse_args()
    pos_src = args.camera_root / "positives"
    neg_src = args.camera_root / "negatives"
    metadata_path = args.camera_root / "BIRDS.TXT"

    reset_dir(args.pos_out)
    reset_dir(args.neg_out)
    args.manifest_out.parent.mkdir(parents=True, exist_ok=True)

    metadata = parse_metadata(metadata_path) if metadata_path.exists() else {}
    pos_names = copy_labeled_images(pos_src, args.pos_out)
    neg_names = copy_labeled_images(neg_src, args.neg_out)

    with args.manifest_out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "filename",
                "label",
                "source",
                "date",
                "datetime",
                "confidence",
                "timestamp",
                "raw_path",
            ],
        )
        writer.writeheader()
        for label, names, out_dir in [
            ("bird", pos_names, args.pos_out),
            ("not_bird", neg_names, args.neg_out),
        ]:
            for name in names:
                meta = summarize_metadata(metadata.get(name, []))
                writer.writerow(
                    {
                        "filename": name,
                        "label": label,
                        "source": "esp32",
                        "date": meta["date"],
                        "datetime": meta["datetime"],
                        "confidence": meta["confidence"],
                        "timestamp": meta["timestamp"],
                        "raw_path": str(out_dir / name),
                    }
                )

    print("Imported ESP32 camera data")
    print("  positives:", len(pos_names), "->", args.pos_out)
    print("  negatives:", len(neg_names), "->", args.neg_out)
    print("  manifest :", args.manifest_out)


if __name__ == "__main__":
    main()
