import csv
import random
import shutil
from collections import defaultdict
from pathlib import Path

import imagehash
from PIL import Image
from sklearn.model_selection import train_test_split
from tqdm import tqdm


RND = random.Random(42)

# ---- RAW DATA SOURCES ----
RAW_POS_PUBLIC = Path("dataset/raw/pos_bird_all")
RAW_POS_ESP32 = Path("dataset/raw/pos_bird_esp32")
RAW_NEG_PUBLIC = Path("dataset/raw/neg_not_bird_all")
RAW_NEG_GARDEN_96 = Path("own_images/garden_negatives")
RAW_NEG_ESP32 = Path("dataset/raw/neg_not_bird_esp32")
ESP32_MANIFEST = Path("dataset/raw/esp32_camera_manifest.csv")

# ---- OUTPUT ----
OUT = Path("dataset/bird_not_bird_96x96_reconstructed")
MANIFEST_DIR = OUT / "manifests"
IMG_SIZE = 96

# ---- DATASET SHAPE ----
MAX_PER_CLASS = 5000
POS_ESP32_FRACTION = 0.15
NEG_PUBLIC_FRACTION = 0.60
NEG_ESP32_FRACTION = 0.25
NEG_GARDEN_FRACTION = 0.15
OVERSAMPLE_ESP32_POS_TRAIN = True

# ---- ESP32 SPLIT POLICY ----
ESP32_TEST_DAYS = {"2026-03-16"}
ESP32_VAL_FRACTION = 0.20

# ---- GARDEN DEDUP ----
DEDUP_GARDEN = True
DEDUP_MAX_HAMMING = 8


def list_images(folder: Path):
    if not folder.exists():
        return []
    exts = {".jpg", ".jpeg", ".png"}
    return sorted([p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in exts])


def center_crop_square(img: Image.Image) -> Image.Image:
    w, h = img.size
    m = min(w, h)
    left = (w - m) // 2
    top = (h - m) // 2
    return img.crop((left, top, left + m, top + m))


def prep_img(path: Path) -> Image.Image:
    img = Image.open(path).convert("RGB")
    img = center_crop_square(img)
    img = img.resize((IMG_SIZE, IMG_SIZE), Image.BILINEAR)
    return img


def sample_without_replacement(paths, k):
    if k <= 0:
        return []
    if len(paths) <= k:
        return paths.copy()
    return RND.sample(paths, k)


def sample_with_optional_oversample(paths, k, oversample=False):
    if k <= 0 or not paths:
        return []
    if len(paths) >= k:
        return RND.sample(paths, k)
    out = paths.copy()
    if oversample:
        out.extend(RND.choices(paths, k=k - len(out)))
    return out


def get_video_stem(path: Path) -> str:
    parts = path.stem.split("_")
    if len(parts) < 2:
        return path.stem
    return "_".join(parts[:-1])


def frame_index(path: Path) -> int:
    last = path.stem.split("_")[-1]
    return int(last) if last.isdigit() else 0


def deduplicate_garden(neg_garden, max_hamming=8):
    video_groups = defaultdict(list)
    for p in neg_garden:
        video_groups[get_video_stem(p)].append(p)

    kept_total = []
    for stem, frames in sorted(video_groups.items()):
        frames_sorted = sorted(frames, key=frame_index)
        kept = []
        last_hash = None
        for p in frames_sorted:
            h = imagehash.phash(Image.open(p).convert("RGB"))
            if last_hash is None or (h - last_hash) > max_hamming:
                kept.append(p)
                last_hash = h
        print(f"  [{stem}] {len(frames)} frames -> {len(kept)} unique")
        kept_total.extend(kept)

    print(
        f"Dedup: {len(neg_garden)} -> {len(kept_total)} garden frames "
        f"(removed {len(neg_garden) - len(kept_total)} near-duplicates)"
    )
    return kept_total


def split_garden_by_video(neg_garden):
    video_groups = defaultdict(list)
    for p in neg_garden:
        video_groups[get_video_stem(p)].append(p)

    video_stems = sorted(video_groups.keys())
    n = len(video_stems)
    if n < 3:
        print(f"WARNING: Only {n} unique garden video source(s) found.")
        if n == 0:
            return {"train": [], "val": [], "test": []}
        if n == 1:
            return {"train": list(video_groups[video_stems[0]]), "val": [], "test": []}
        return {
            "train": list(video_groups[video_stems[0]]),
            "val": list(video_groups[video_stems[1]]),
            "test": [],
        }

    stems_train, stems_tmp = train_test_split(video_stems, test_size=0.2, random_state=42)
    stems_val, stems_test = train_test_split(stems_tmp, test_size=0.5, random_state=42)

    return {
        "train": [p for s in stems_train for p in video_groups[s]],
        "val": [p for s in stems_val for p in video_groups[s]],
        "test": [p for s in stems_test for p in video_groups[s]],
    }


def random_split(paths):
    if not paths:
        return {"train": [], "val": [], "test": []}
    train_paths, tmp = train_test_split(paths, test_size=0.2, random_state=42)
    val_paths, test_paths = train_test_split(tmp, test_size=0.5, random_state=42)
    return {"train": train_paths, "val": val_paths, "test": test_paths}


def load_esp32_manifest(manifest_path: Path):
    rows = []
    if not manifest_path.exists():
        return rows
    with manifest_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    return rows


def split_esp32(rows, expected_label, base_dir: Path):
    by_split = {"train": [], "val": [], "test": []}
    remaining_by_day = defaultdict(list)

    for row in rows:
        if row.get("label") != expected_label:
            continue
        path = base_dir / row["filename"]
        if not path.exists():
            continue
        date = row.get("date", "")
        if date in ESP32_TEST_DAYS:
            by_split["test"].append({"path": path, "source": "esp32", "group": date})
        else:
            remaining_by_day[date].append({"path": path, "source": "esp32", "group": date})

    for date, samples in sorted(remaining_by_day.items()):
        if not samples:
            continue
        if date:
            val_count = int(round(len(samples) * ESP32_VAL_FRACTION))
            if len(samples) >= 5:
                val_count = max(1, val_count)
        else:
            val_count = 0
        val_count = min(val_count, len(samples))
        val_samples = pick_from_pool(samples, val_count, oversample=False)
        val_paths = {item["path"] for item in val_samples}
        train_samples = [item for item in samples if item["path"] not in val_paths]
        by_split["val"].extend(val_samples)
        by_split["train"].extend(train_samples)

    return by_split


def wrap_samples(paths, source, group_fn=None):
    samples = []
    for path in paths:
        group = group_fn(path) if group_fn else ""
        samples.append({"path": path, "source": source, "group": group})
    return samples


def pick_from_pool(samples, k, oversample=False):
    if k <= 0 or not samples:
        return []
    if len(samples) >= k:
        return RND.sample(samples, k)
    out = samples.copy()
    if oversample:
        out.extend(RND.choices(samples, k=k - len(out)))
    return out


def build_split_mix(target_total, source_configs):
    chosen = []
    work_configs = []
    for cfg in source_configs:
        work_configs.append(
            {
                "samples": cfg["samples"].copy(),
                "fraction": cfg["fraction"],
                "oversample": cfg.get("oversample", False),
                "oversample_fill": cfg.get("oversample_fill", False),
            }
        )

    for cfg in work_configs:
        target = int(round(target_total * cfg["fraction"]))
        picked = pick_from_pool(cfg["samples"], target, oversample=cfg.get("oversample", False))
        chosen.extend(picked)
        if not cfg.get("oversample", False):
            picked_paths = {item["path"] for item in picked}
            cfg["samples"] = [item for item in cfg["samples"] if item["path"] not in picked_paths]

    remaining = target_total - len(chosen)
    if remaining > 0:
        for cfg in work_configs:
            if remaining <= 0:
                break
            available = cfg["samples"]
            if not available:
                continue
            extra = pick_from_pool(available, remaining, oversample=cfg.get("oversample_fill", False))
            chosen.extend(extra)
            if not cfg.get("oversample_fill", False):
                extra_paths = {item["path"] for item in extra}
                cfg["samples"] = [item for item in cfg["samples"] if item["path"] not in extra_paths]
            remaining = target_total - len(chosen)

    if len(chosen) > target_total:
        chosen = RND.sample(chosen, target_total)

    RND.shuffle(chosen)
    return chosen


def save_split(samples, label, split):
    out_dir = OUT / split / label
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_rows = []
    name_count = {}

    for sample in tqdm(samples, desc=f"Saving {split}/{label}"):
        path = sample["path"]
        img = prep_img(path)
        count = name_count.get(path.stem, 0)
        fname = path.name if count == 0 else f"{path.stem}_dup{count}{path.suffix.lower()}"
        name_count[path.stem] = count + 1
        img.save(out_dir / fname, quality=95)
        manifest_rows.append(
            {
                "split": split,
                "label": label,
                "source": sample["source"],
                "group": sample["group"],
                "original_path": str(path),
                "saved_name": fname,
            }
        )
    return manifest_rows


def write_manifest(rows):
    MANIFEST_DIR.mkdir(parents=True, exist_ok=True)
    out_path = MANIFEST_DIR / "split_manifest.csv"
    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["split", "label", "source", "group", "original_path", "saved_name"],
        )
        writer.writeheader()
        writer.writerows(rows)
    print("Saved split manifest to", out_path)


def source_breakdown(rows):
    counts = defaultdict(int)
    for row in rows:
        counts[row["source"]] += 1
    return dict(sorted(counts.items()))


def main():
    if OUT.exists():
        shutil.rmtree(OUT)

    pos_public = list_images(RAW_POS_PUBLIC)
    neg_public = list_images(RAW_NEG_PUBLIC)
    neg_garden = list_images(RAW_NEG_GARDEN_96)
    esp32_rows = load_esp32_manifest(ESP32_MANIFEST)

    if not pos_public:
        raise RuntimeError(f"No public positives found in {RAW_POS_PUBLIC}")
    if not neg_public and not neg_garden:
        raise RuntimeError("No negatives found.")

    if neg_garden and DEDUP_GARDEN:
        print(f"\nDeduplicating garden frames (max_hamming={DEDUP_MAX_HAMMING})...")
        neg_garden = deduplicate_garden(neg_garden, max_hamming=DEDUP_MAX_HAMMING)

    pos_public_split = {
        split: wrap_samples(paths, "public")
        for split, paths in random_split(pos_public).items()
    }
    neg_public_split = {
        split: wrap_samples(paths, "public")
        for split, paths in random_split(neg_public).items()
    }
    neg_garden_split = {
        split: wrap_samples(paths, "garden", group_fn=get_video_stem)
        for split, paths in split_garden_by_video(neg_garden).items()
    }
    pos_esp32_split = split_esp32(esp32_rows, "bird", RAW_POS_ESP32)
    neg_esp32_split = split_esp32(esp32_rows, "not_bird", RAW_NEG_ESP32)

    n_pos_available = len(pos_public) + len(list_images(RAW_POS_ESP32))
    n_neg_available = len(neg_public) + len(neg_garden) + len(list_images(RAW_NEG_ESP32))
    n_final = min(MAX_PER_CLASS, n_pos_available, n_neg_available)

    n_train = int(round(n_final * 0.8))
    n_val = int(round(n_final * 0.1))
    n_test = n_final - n_train - n_val
    split_targets = {"train": n_train, "val": n_val, "test": n_test}

    all_manifest_rows = []
    for split, target in split_targets.items():
        pos_samples = build_split_mix(
            target,
            [
                {
                    "samples": pos_public_split[split],
                    "fraction": 1.0 - POS_ESP32_FRACTION,
                    "oversample_fill": False,
                },
                {
                    "samples": pos_esp32_split[split],
                    "fraction": POS_ESP32_FRACTION,
                    "oversample": split == "train" and OVERSAMPLE_ESP32_POS_TRAIN,
                    "oversample_fill": split == "train" and OVERSAMPLE_ESP32_POS_TRAIN,
                },
            ],
        )
        neg_samples = build_split_mix(
            target,
            [
                {"samples": neg_public_split[split], "fraction": NEG_PUBLIC_FRACTION},
                {"samples": neg_esp32_split[split], "fraction": NEG_ESP32_FRACTION},
                {"samples": neg_garden_split[split], "fraction": NEG_GARDEN_FRACTION},
            ],
        )

        print(f"\n{split.upper()} target: {target}")
        print("  positives:", len(pos_samples), source_breakdown(pos_samples))
        print("  negatives:", len(neg_samples), source_breakdown(neg_samples))

        all_manifest_rows.extend(save_split(pos_samples, "bird", split))
        all_manifest_rows.extend(save_split(neg_samples, "not_bird", split))

    write_manifest(all_manifest_rows)
    print("\nDone. Dataset created at:", OUT)


if __name__ == "__main__":
    main()
