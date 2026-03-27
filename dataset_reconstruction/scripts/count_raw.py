from pathlib import Path

def count_images(folder):
    p = Path(folder)
    if not p.exists():
        return 0
    exts = {".jpg", ".jpeg", ".png"}
    return sum(1 for f in p.iterdir() if f.suffix.lower() in exts)

print("pos_bird_all:", count_images("dataset/raw/pos_bird_all"))
print("pos_bird_coco:", count_images("dataset/raw/pos_bird"))
print("pos_bird_cub:", count_images("dataset/raw/pos_bird_cub_selected"))
print("pos_bird_ena24:", count_images("dataset/raw/pos_bird_ena24"))
print("pos_bird_esp32:", count_images("dataset/raw/pos_bird_esp32"))
print("neg_not_bird:", count_images("dataset/raw/neg_not_bird"))
print("neg_not_bird_ena24:", count_images("dataset/raw/neg_not_bird_ena24"))
print("neg_not_bird_cct20:", count_images("dataset/raw/neg_not_bird_cct20"))
print("neg_not_bird_all:", count_images("dataset/raw/neg_not_bird_all"))
print("neg_not_bird_garden:", count_images("dataset/raw/neg_not_bird_garden"))
print("neg_not_bird_garden_96:", count_images("dataset/raw/neg_not_bird_garden_96"))
print("neg_not_bird_esp32:", count_images("dataset/raw/neg_not_bird_esp32"))
