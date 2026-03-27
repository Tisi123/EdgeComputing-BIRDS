from pathlib import Path
import shutil
from tqdm import tqdm

COCO_DIR = Path("dataset/raw/pos_bird")
CUB_DIR = Path("dataset/raw/pos_bird_cub_selected")
ENA24_DIR = Path("dataset/raw/pos_bird_ena24")

OUT_DIR = Path("dataset/raw/pos_bird_all")
if OUT_DIR.exists():
    shutil.rmtree(OUT_DIR)
OUT_DIR.mkdir(parents=True)


def copy_images(src_dir, prefix):

    paths = list(src_dir.glob("*.jpg"))

    for i,p in enumerate(tqdm(paths,desc=prefix)):

        newname=f"{prefix}_{i}.jpg"

        shutil.copy(p, OUT_DIR/newname)



print("Copying COCO birds...")
copy_images(COCO_DIR,"coco")

print("Copying CUB birds...")
copy_images(CUB_DIR,"cub")

if ENA24_DIR.exists():
    print("Copying ENA24 birds...")
    copy_images(ENA24_DIR, "ena24")
else:
    print("Skipping ENA24 birds (folder not found):", ENA24_DIR)


print("Done")

print("Total merged:",
len(list(OUT_DIR.glob("*.jpg"))))
