import argparse
import subprocess
import sys
from pathlib import Path


def run_step(cmd):
    print("\n>>>", " ".join(str(x) for x in cmd))
    subprocess.run(cmd, check=True)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Orchestrate bird detector data pipeline with optional LILA sources."
    )
    parser.add_argument("--python", default=sys.executable, help="Python executable to use.")
    parser.add_argument("--with-ena24", action="store_true", help="Include LILA ENA24 extraction steps (required for exact reproduction).")
    parser.add_argument(
        "--with-own-images",
        action="store_true",
        help="Import our own ESP32 images from own_images/ into the raw pipeline.",
    )
    parser.add_argument("--skip-base", action="store_true", help="Skip base COCO/CUB/garden extraction steps.")
    return parser.parse_args()


def main():
    args = parse_args()
    py = args.python

    if not args.skip_base:
        run_step([py, "scripts/extract_coco_birds.py"])
        run_step([py, "scripts/extract_selected_cub_species.py"])
        run_step([py, "scripts/merge_positive_datasets.py"])
        run_step([py, "scripts/extract_coco_negatives.py"])
        run_step([py, "scripts/extract_garden_frames.py"])
        run_step([py, "scripts/prep_garden_negatives_96.py"])

    if args.with_ena24:
        run_step(
            [
                py,
                "scripts/extract_lila_camera_trap_positives.py",
                "--metadata-json",
                "dataset/external/lila/ena24/metadata.json",
                "--images-root",
                "dataset/external/lila/ena24/images",
                "--out-dir",
                "dataset/raw/pos_bird_ena24",
                "--dataset-tag",
                "ena24",
                "--target-label-mode",
                "strict",
            ]
        )
        run_step(
            [
                py,
                "scripts/extract_lila_camera_trap_negatives.py",
                "--metadata-json",
                "dataset/external/lila/ena24/metadata.json",
                "--images-root",
                "dataset/external/lila/ena24/images",
                "--out-dir",
                "dataset/raw/neg_not_bird_ena24",
                "--dataset-tag",
                "ena24",
                "--negative-mode",
                "empty_plus_non_bird",
            ]
        )
        run_step([py, "scripts/merge_positive_datasets.py"])

    if args.with_own_images:
        run_step([py, "scripts/import_esp32_camera_data.py"])

    sources = [
        "coco=dataset/raw/neg_not_bird",
        "garden=own_images/garden_negatives",
    ]
    if args.with_ena24 and Path("dataset/raw/neg_not_bird_ena24").exists():
        sources.append("ena24=dataset/raw/neg_not_bird_ena24")

    merge_cmd = [py, "scripts/merge_negative_datasets.py"]
    for src in sources:
        merge_cmd.extend(["--source", src])
    merge_cmd.extend(["--out-dir", "dataset/raw/neg_not_bird_all"])
    run_step(merge_cmd)

    run_step([py, "scripts/count_raw.py"])
    run_step([py, "scripts/build_dataset_96.py"])
    run_step([py, "scripts/count_processed.py", "--base", "dataset/bird_not_bird_96x96_reconstructed"])

    print("\nPipeline complete.")


if __name__ == "__main__":
    main()
