import shutil
from pathlib import Path
from PIL import Image
from tqdm import tqdm

CUB_ROOT = Path("CUB_200_2011")
IMG_DIR = CUB_ROOT / "images"

OUT_DIR = Path("dataset/raw/pos_bird_cub_selected")
if OUT_DIR.exists():
    shutil.rmtree(OUT_DIR)
OUT_DIR.mkdir(parents=True)


SELECTED_CLASSES = {

9,10,11,12,

28,
34,
35,
47,
48,

58,

76,

94,

113,
116,
118,
119,
120,
125,
127,
129,
130,
132,
133,

134,

149,
150,
185,
186,

193,
196,
199

}


def load_classes():

    d={}

    with open(CUB_ROOT/"image_class_labels.txt") as f:

        for line in f:

            img_id,class_id=line.strip().split()

            d[int(img_id)]=int(class_id)

    return d



def load_images():

    d={}

    with open(CUB_ROOT/"images.txt") as f:

        for line in f:

            img_id,path=line.strip().split()

            d[int(img_id)]=path

    return d


classes=load_classes()
images=load_images()


saved=0

for img_id in tqdm(images):

    if classes[img_id] not in SELECTED_CLASSES:
        continue

    img_path=IMG_DIR/images[img_id]

    img=Image.open(img_path).convert("RGB")

    outname=f"cub_sel_{img_id}.jpg"

    img.save(OUT_DIR/outname,quality=95)

    saved+=1


print("Saved:",saved)