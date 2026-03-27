import shutil
from pathlib import Path
from PIL import Image
from tqdm import tqdm

IN_DIR = Path("dataset/raw/neg_not_bird_garden")
OUT_DIR = Path("dataset/raw/neg_not_bird_garden_96")
IMG_SIZE = 96

def center_crop_square(img: Image.Image) -> Image.Image:
    w, h = img.size
    m = min(w, h)
    left = (w - m) // 2
    top = (h - m) // 2
    return img.crop((left, top, left + m, top + m))

if OUT_DIR.exists():
    shutil.rmtree(OUT_DIR)
OUT_DIR.mkdir(parents=True)

paths = sorted([p for p in IN_DIR.glob("*.jpg")])
for p in tqdm(paths, desc="Resizing garden negatives to 96x96"):
    img = Image.open(p).convert("RGB")
    img = center_crop_square(img).resize((IMG_SIZE, IMG_SIZE), Image.BILINEAR)
    img.save(OUT_DIR / p.name, quality=95)

print("Saved:", len(paths), "to", OUT_DIR)