import json
import shutil
from pathlib import Path
from PIL import Image
from tqdm import tqdm
from pycocotools.coco import COCO

# ---- config ----
COCO_ROOT = Path("coco")  # contains train2017/, val2017/, annotations/
OUT_DIR = Path("dataset/raw/pos_bird")
SPLITS = ["train2017", "val2017"]

PAD_FRAC = 0.10          # pad bbox by 10% for context
MIN_BOX_SIZE = 20        # ignore tiny boxes (in pixels)

if OUT_DIR.exists():
    shutil.rmtree(OUT_DIR)
OUT_DIR.mkdir(parents=True)

def load_coco_utf8(ann_path: Path) -> COCO:
    coco = COCO()
    with open(ann_path, "r", encoding="utf-8") as f:
        coco.dataset = json.load(f)
    coco.createIndex()
    return coco

def clamp(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))

def crop_with_padding(img: Image.Image, bbox, pad_frac: float):
    # bbox = [x, y, w, h]
    x, y, w, h = bbox
    if w < MIN_BOX_SIZE or h < MIN_BOX_SIZE:
        return None

    pad_w = w * pad_frac
    pad_h = h * pad_frac

    left = int(x - pad_w)
    top = int(y - pad_h)
    right = int(x + w + pad_w)
    bottom = int(y + h + pad_h)

    left = clamp(left, 0, img.width)
    top = clamp(top, 0, img.height)
    right = clamp(right, 0, img.width)
    bottom = clamp(bottom, 0, img.height)

    if right - left < MIN_BOX_SIZE or bottom - top < MIN_BOX_SIZE:
        return None

    return img.crop((left, top, right, bottom))

def process_split(split_name: str) -> int:
    ann_path = COCO_ROOT / "annotations" / f"instances_{split_name}.json"
    img_dir = COCO_ROOT / split_name

    if not ann_path.exists():
        raise FileNotFoundError(f"Missing annotation file: {ann_path}")
    if not img_dir.exists():
        raise FileNotFoundError(f"Missing image directory: {img_dir}")

    coco = load_coco_utf8(ann_path)

    cat_ids = coco.getCatIds(catNms=["bird"])
    if not cat_ids:
        raise RuntimeError("Could not find COCO category 'bird' in annotations.")
    bird_cat_id = cat_ids[0]

    img_ids = coco.getImgIds(catIds=[bird_cat_id])

    saved = 0
    for img_id in tqdm(img_ids, desc=f"Extracting birds from {split_name}"):
        info = coco.loadImgs(img_id)[0]
        img_path = img_dir / info["file_name"]
        if not img_path.exists():
            continue

        img = Image.open(img_path).convert("RGB")

        ann_ids = coco.getAnnIds(imgIds=[img_id], catIds=[bird_cat_id], iscrowd=None)
        anns = coco.loadAnns(ann_ids)

        for j, ann in enumerate(anns):
            crop = crop_with_padding(img, ann["bbox"], PAD_FRAC)
            if crop is None:
                continue
            out_name = f"{split_name}_{img_id}_{j}.jpg"
            crop.save(OUT_DIR / out_name, quality=95)
            saved += 1

    print(f"{split_name}: saved {saved} bird crops to {OUT_DIR}")
    return saved

if __name__ == "__main__":
    total = 0
    for s in SPLITS:
        total += process_split(s)
    print(f"Total bird crops saved: {total}")