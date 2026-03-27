# Bird / Not-Bird 96×96 Dataset Reconstruction

Reproducibility package for the binary bird-detection dataset used to train the BIRDS ESP32-S3 TinyML detector.

This folder contains:
- the pipeline scripts to reconstruct the full dataset from scratch
- **only our own collected images**, which are the only assets we are licensed to redistribute
- a provenance metadata file for those images

Third-party datasets (COCO, CUB-200-2011, LILA ENA24) are **not included** and must be downloaded separately before running the pipeline.

---

## Dataset overview

| Property         | Value                                       |
|-----------------|----------------------------------------------|
| Task             | Binary classification: bird vs. not\_bird   |
| Image size       | 96 × 96 pixels, RGB JPEG                    |
| Total images     | 10 000                                       |
| Positive class   | bird (label = 1)                             |
| Negative class   | not\_bird (label = 0)                        |
| Output directory | `dataset/bird_not_bird_96x96_reconstructed/` |

### Split breakdown

| Split | bird | not\_bird | total |
|-------|------|-----------|-------|
| train | 4000 | 4000      | 8000  |
| val   | 500  | 500       | 1000  |
| test  | 500  | 500       | 1000  |

---

## Our own redistributed images

**Location:** `own_images/`

These are the only image assets committed to this repository.
All images were captured by us using an ESP32-S3-EYE garden camera.
They are redistributed here under the same license as this repository.

| Folder                        | Count | Label     | Description                              |
|-------------------------------|-------|-----------|------------------------------------------|
| `own_images/positives/`       | 175   | bird      | ESP32 garden camera bird images          |
| `own_images/negatives/`       | 830   | not\_bird | ESP32 garden camera not-bird images      |
| `own_images/garden_negatives/`| 355   | not\_bird | Pre-processed 96×96 garden video frames  |

**Provenance file:** `own_images/metadata.csv`

Fields: `filename`, `label`, `source`, `date`, `datetime`, `confidence`, `redistributed_by_us`, `notes`

- `source`: `esp32` (labeled camera images) or `garden` (video frames)
- `confidence`: ESP32 model confidence score at capture time (empty for garden frames)
- `redistributed_by_us`: always `true` for every row in this file

---

## Third-party datasets (NOT included — must be downloaded)

You must obtain and place these datasets before running the pipeline.

### MS COCO 2017

Used for: bird crops (positives) and non-bird images (negatives).

- Download from: https://cocodataset.org/#download
- Files needed: `train2017.zip`, `val2017.zip`, `annotations_trainval2017.zip`
- Place at:
  ```
  dataset_reconstruction/
  └── coco/
      ├── train2017/
      ├── val2017/
      └── annotations/
          ├── instances_train2017.json
          └── instances_val2017.json
  ```

### CUB-200-2011 (Caltech-UCSD Birds)

Used for: additional bird positive samples (47 selected species out of 200).

- Download from: https://www.vision.caltech.edu/datasets/cub_200_2011/
- File needed: `CUB_200_2011.tgz`
- Place at:
  ```
  dataset_reconstruction/
  └── CUB_200_2011/
      └── images/
  ```

### LILA ENA24

Used for: additional bird positives (1833 images) and non-bird negatives from camera traps.
**Required** to reproduce the exact original dataset.

- Download from: https://lila.science/datasets/ena24detection
- Place at:
  ```
  dataset_reconstruction/
  └── dataset/external/lila/ena24/
      ├── metadata.json
      └── images/
  ```

---

## Raw data layout (before running pipeline)

```
dataset_reconstruction/
├── coco/                              # MS COCO 2017 (user provides)
│   ├── train2017/
│   ├── val2017/
│   └── annotations/
├── CUB_200_2011/                      # CUB-200-2011 (user provides)
│   └── images/
├── dataset/
│   └── external/lila/ena24/           # LILA ENA24 (user provides, required)
├── raw_videos/                        # Garden videos (user provides, only needed
│                                      # if regenerating garden frames from scratch;
│                                      # pre-processed frames already in own_images/)
├── own_images/                        # Our redistributable images (included)
│   ├── positives/
│   ├── negatives/
│   ├── garden_negatives/
│   └── metadata.csv
├── scripts/                           # Pipeline scripts
└── run_pipeline.py                    # Entry point
```

---

## Requirements

Python 3.9+ is recommended.

```
pip install pillow imagehash scikit-learn tqdm pycocotools opencv-python
```

> **Note:** `pycocotools` requires a C compiler on Windows. Install via
> `pip install pycocotools-windows` if the standard package fails to build.

---

## Quick start

Run all commands from inside the `dataset_reconstruction/` directory.

```bash
cd dataset_reconstruction/

# Full reconstruction matching the original dataset (COCO + CUB + ENA24 + own images)
python run_pipeline.py --with-own-images --with-ena24
```

The final dataset is written to:
```
dataset/bird_not_bird_96x96_reconstructed/
├── train/
│   ├── bird/
│   └── not_bird/
├── val/
│   ├── bird/
│   └── not_bird/
├── test/
│   ├── bird/
│   └── not_bird/
└── manifests/
    └── split_manifest.csv
```

---

## Pipeline flags

| Flag               | Description                                              |
|--------------------|----------------------------------------------------------|
| `--with-own-images`| Import ESP32 images from `own_images/` into raw pipeline |
| `--with-ena24`     | Include LILA ENA24 camera trap data (used in original)   |
| `--skip-base`      | Skip COCO/CUB/garden extraction (reuse existing raw/)    |
| `--python PATH`    | Override the Python executable                           |

---

## Step-by-step manual execution

If you prefer to run steps individually, execute from `dataset_reconstruction/`:

```bash
# 1. Extract COCO bird crops → dataset/raw/pos_bird/
python scripts/extract_coco_birds.py

# 2. Extract selected CUB species → dataset/raw/pos_bird_cub_selected/
python scripts/extract_selected_cub_species.py

# 3. Extract LILA ENA24 bird positives → dataset/raw/pos_bird_ena24/
python scripts/extract_lila_camera_trap_positives.py \
  --metadata-json dataset/external/lila/ena24/metadata.json \
  --images-root dataset/external/lila/ena24/images \
  --out-dir dataset/raw/pos_bird_ena24 \
  --dataset-tag ena24 \
  --target-label-mode strict

# 4. Merge all public positives → dataset/raw/pos_bird_all/
python scripts/merge_positive_datasets.py

# 5. Extract COCO non-bird images → dataset/raw/neg_not_bird/
python scripts/extract_coco_negatives.py

# 5b. Extract LILA ENA24 non-bird negatives → dataset/raw/neg_not_bird_ena24/
python scripts/extract_lila_camera_trap_negatives.py \
  --metadata-json dataset/external/lila/ena24/metadata.json \
  --images-root dataset/external/lila/ena24/images \
  --out-dir dataset/raw/neg_not_bird_ena24 \
  --dataset-tag ena24 \
  --negative-mode empty_plus_non_bird

# 6. (Optional — skip if using pre-processed own_images/garden_negatives/)
#    Extract garden video frames → dataset/raw/neg_not_bird_garden/
python scripts/extract_garden_frames.py
#    Resize garden frames to 96x96 → dataset/raw/neg_not_bird_garden_96/
python scripts/prep_garden_negatives_96.py

# 7. Import our own ESP32 images → dataset/raw/pos_bird_esp32/ and neg_not_bird_esp32/
python scripts/import_esp32_camera_data.py

# 8. Merge all negatives → dataset/raw/neg_not_bird_all/
python scripts/merge_negative_datasets.py \
  --source coco=dataset/raw/neg_not_bird \
  --source garden=own_images/garden_negatives \
  --source ena24=dataset/raw/neg_not_bird_ena24 \
  --out-dir dataset/raw/neg_not_bird_all

# 9. Compose the final dataset → dataset/bird_not_bird_96x96_reconstructed/
python scripts/build_dataset_96.py

# 10. Verify counts
python scripts/count_processed.py
```

> **Garden frames note:** Steps 5a and 5b are only required if you want to
> regenerate garden frames from source video. The pre-processed 96×96 frames
> are already provided in `own_images/garden_negatives/` and are used directly
> by `build_dataset_96.py` — no re-extraction needed for a standard run.

---

## Output files

After a successful run, the following are generated inside `dataset/`:

```
dataset/bird_not_bird_96x96_reconstructed/
├── train/bird/          4000 JPEG images (96×96)
├── train/not_bird/      4000 JPEG images (96×96)
├── val/bird/             500 JPEG images (96×96)
├── val/not_bird/         500 JPEG images (96×96)
├── test/bird/            500 JPEG images (96×96)
├── test/not_bird/        500 JPEG images (96×96)
└── manifests/
    └── split_manifest.csv   (10 000 rows)

dataset/raw/                 (intermediate extraction outputs — can be deleted after build)
```

### split_manifest.csv schema

| Field           | Description                                                  |
|-----------------|--------------------------------------------------------------|
| `split`         | `train` / `val` / `test`                                     |
| `label`         | `bird` / `not_bird`                                          |
| `source`        | `public` (COCO/CUB/LILA) / `esp32` / `garden`               |
| `group`         | Video stem (garden) or capture date (ESP32), empty otherwise |
| `original_path` | Absolute path to source image at build time                  |
| `saved_name`    | Filename in the output split folder                          |

---

## Reproducibility details

All randomness is seeded for exact reproducibility:

- `RND = random.Random(42)` — used throughout `build_dataset_96.py`
- `sklearn.model_selection.train_test_split(random_state=42)` — used for public and garden splits

### Dataset composition

Actual image counts from the original build (sourced from `split_manifest.csv`):

| Class     | Source                         | Count | % of class |
|-----------|--------------------------------|-------|------------|
| bird      | COCO + CUB + ENA24 (public)    | 4308  | 86.2%      |
| bird      | ESP32 camera                   |  692  | 13.8%      |
| not\_bird | COCO + ENA24 (public)          | 4008  | 80.2%      |
| not\_bird | ESP32 camera                   |  784  | 15.7%      |
| not\_bird | Garden video frames            |  208  |  4.2%      |

> **Note:** The pipeline targets 85/15% for birds and 60/25/15% for not\_bird, but actual
> fractions differ because garden frames are limited by what survived deduplication (208 unique
> frames), and the pipeline fills shortfalls from other sources.

### Key parameters in `scripts/build_dataset_96.py`

```python
MAX_PER_CLASS = 5000          # images per class
IMG_SIZE = 96                 # output resolution
OVERSAMPLE_ESP32_POS_TRAIN = True   # oversample ESP32 birds in train split
ESP32_TEST_DAYS = {"2026-03-16"}    # dates held out exclusively for test
ESP32_VAL_FRACTION = 0.20           # fraction of remaining ESP32 data for val
DEDUP_GARDEN = True                 # deduplicate garden frames by perceptual hash
DEDUP_MAX_HAMMING = 8               # max hash distance to consider as duplicate
```

### Image processing

All images are processed identically:
1. Open as RGB
2. Center-crop to square (minimum dimension)
3. Bilinear resize to 96 × 96
4. Save as JPEG (quality=95)

### Garden frame deduplication

Garden video frames are deduplicated per video source using perceptual hashing (`imagehash.phash`). Frames within the same video whose hash differs by ≤ 8 (Hamming distance) from the previous kept frame are dropped. This prevents near-duplicate consecutive frames from dominating the negatives.

### ESP32 test holdout

Images captured on dates listed in `ESP32_TEST_DAYS` are assigned exclusively to the test split. This ensures that model evaluation on the test set reflects performance on a distinct recording session.

---

## Provenance and source transparency

| Source            | Used as    | License                                  | Redistributed here |
|-------------------|------------|------------------------------------------|--------------------|
| MS COCO 2017      | pos + neg  | CC BY 4.0 (annotations); images: Flickr | No                 |
| CUB-200-2011      | pos        | Research use (Caltech)                   | No                 |
| LILA ENA24        | pos + neg  | Various (see LILA)                       | No                 |
| ESP32 camera      | pos + neg  | Collected by us                          | **Yes**            |
| Garden video      | neg        | Collected by us                          | **Yes**            |

Our own images (`own_images/`) are the **only** image assets committed to this repository.
All provenance details for these images are in `own_images/metadata.csv`.

---

## License

Pipeline scripts: MIT License (see repository root `LICENSE` if present).

Our own images in `own_images/`: released under [**CC BY 4.0**](https://creativecommons.org/licenses/by/4.0/).
Attribution: BIRDS project, ESP32 garden camera dataset.

Third-party images are subject to their respective licenses — see the links in the
"Third-party datasets" section above.
