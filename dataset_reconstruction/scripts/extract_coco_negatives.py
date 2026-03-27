import json
import random
import shutil
from pathlib import Path
from PIL import Image
from tqdm import tqdm
from pycocotools.coco import COCO

# ---- config ----
COCO_ROOT = Path("coco")
OUT_DIR = Path("dataset/raw/neg_not_bird")
SPLITS = ["train2017", "val2017"]

N_NEG_IMAGES = 10000       # how many negatives total you want
RESIZE_LONG_EDGE = 320    # reduce storage (keeps aspect ratio)

random.seed(42)
if OUT_DIR.exists():
    shutil.rmtree(OUT_DIR)
OUT_DIR.mkdir(parents=True)

def load_coco_utf8(ann_path: Path) -> COCO:
    coco = COCO()
    with open(ann_path, "r", encoding="utf-8") as f:
        coco.dataset = json.load(f)
    coco.createIndex()
    return coco

def resize_keep_aspect(img: Image.Image, long_edge: int) -> Image.Image:
    w, h = img.size
    m = max(w, h)
    if m <= long_edge:
        return img
    if w >= h:
        new_w = long_edge
        new_h = int(h * long_edge / w)
    else:
        new_h = long_edge
        new_w = int(w * long_edge / h)
    return img.resize((new_w, new_h), Image.BILINEAR)

def collect_negative_img_ids(coco: COCO) -> list[int]:
    bird_ids = coco.getCatIds(catNms=["bird"])
    if not bird_ids:
        raise RuntimeError("Could not find COCO category 'bird' in annotations.")
    bird_cat_id = bird_ids[0]

    bird_img_ids = set(coco.getImgIds(catIds=[bird_cat_id]))
    all_img_ids = set(coco.getImgIds())
    neg_ids = list(all_img_ids - bird_img_ids)
    return neg_ids

def process_split(split_name: str, remaining_to_save: int) -> int:
    ann_path = COCO_ROOT / "annotations" / f"instances_{split_name}.json"
    img_dir = COCO_ROOT / split_name

    if not ann_path.exists():
        raise FileNotFoundError(f"Missing annotation file: {ann_path}")
    if not img_dir.exists():
        raise FileNotFoundError(f"Missing image directory: {img_dir}")

    coco = load_coco_utf8(ann_path)

    neg_ids = collect_negative_img_ids(coco)
    random.shuffle(neg_ids)

    saved = 0
    for img_id in tqdm(neg_ids, desc=f"Saving negatives from {split_name}"):
        if saved >= remaining_to_save:
            break

        info = coco.loadImgs(img_id)[0]
        img_path = img_dir / info["file_name"]
        if not img_path.exists():
            continue

        img = Image.open(img_path).convert("RGB")
        img = resize_keep_aspect(img, RESIZE_LONG_EDGE)

        out_name = f"{split_name}_{img_id}.jpg"
        img.save(OUT_DIR / out_name, quality=90)
        saved += 1

    print(f"{split_name}: saved {saved} negatives to {OUT_DIR}")
    return saved

if __name__ == "__main__":
    remaining = N_NEG_IMAGES
    total = 0
    for s in SPLITS:
        if remaining <= 0:
            break
        saved = process_split(s, remaining)
        total += saved
        remaining -= saved

    print(f"Total negatives saved: {total}")