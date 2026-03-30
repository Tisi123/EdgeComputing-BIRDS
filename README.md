# BIRDS: Bird vs Not-Bird Detection on an Edge AI Pipeline

BIRDS is an end-to-end edge AI project centered on an ESP32-S3-EYE camera node that performs on-device TinyML inference, stores detections locally, exposes them over Wi-Fi, and forwards summaries through a MoleNet/LoRaWAN path. The deployed model in this repository is a **binary bird vs not-bird detector**, not a species classifier.

## Overview

The repository combines three connected parts of the system:

- **ESP32-S3 firmware** for image capture, TensorFlow Lite Micro inference, SD-card storage, HTTP endpoints, and scheduled sleep/wake behavior
- **Dataset reconstruction and training support** for rebuilding the `96x96` bird/not-bird dataset and retraining/exporting the detector
- **MoleNet integration scripts** for fetching detections over Wi-Fi and forwarding compact payloads via LoRaWAN / TTN

At a high level, the device captures frames, classifies them as `bird` or `not_bird`, stores bird detections, exposes them over HTTP, and lets a MoleNet node retrieve and forward those detections.

## Repository Structure

- [`main/`](main/)  
  ESP32-S3 firmware sources, including inference, storage, scheduling, and HTTP handling

- [`dataset_reconstruction/`](dataset_reconstruction/)  
  Dataset reconstruction pipeline, provenance notes, third-party dataset requirements, and retraining guidance

- [`MoleNet/`](MoleNet/)  
  MicroPython scripts for time sync, data retrieval, payload encoding, and LoRaWAN / TTN transmission

- [`model/`](model/)  
  Versioned exported model artifacts for embedded deployment

- [`plots/`](plots/)  
  Evaluation plots for multiple model versions and version comparisons

## Artifacts and Reproduction

This repo includes committed model artifacts and evaluation plots, including versioned outputs under [`model/`](model/) and [`plots/`](plots/). For the full dataset reconstruction and retraining workflow, use [`dataset_reconstruction/README.md`](dataset_reconstruction/README.md), which documents required third-party datasets, reconstruction commands, redistributed image provenance, and the referenced Colab notebook:

<https://colab.research.google.com/drive/18Sh_G6UzMayeEFLGDky-rUCnaivUEBNi?usp=sharing>

### Run the dataset pipeline

Run the reconstruction pipeline from inside [`dataset_reconstruction/`](dataset_reconstruction/) after placing the required third-party datasets in the expected folders.

```bash
cd dataset_reconstruction
pip install pillow imagehash scikit-learn tqdm pycocotools opencv-python
python run_pipeline.py --with-own-images --with-ena24
```

This rebuilds the `96x96` bird/not-bird dataset used by the embedded detector. The exact dataset layout, optional flags, manual step-by-step execution, and third-party data requirements are documented in [`dataset_reconstruction/README.md`](dataset_reconstruction/README.md).

## Integration Notes

The ESP32 firmware exposes a small HTTP interface used by the MoleNet side:

- `GET /sync_devices_request`
- `GET /get_bird_data_request`
- `POST /flush_to_sd`
- `GET /get_bird_history`

## Important Notes

- This repository currently implements **binary `bird` / `not_bird` detection**, not bird species classification.
- Third-party datasets used in reconstruction are **not** redistributed here.
- The only redistributed image assets in the repo are under [`dataset_reconstruction/own_images/`](dataset_reconstruction/own_images/).
- Detailed operational and provenance information lives in [`dataset_reconstruction/README.md`](dataset_reconstruction/README.md).

The report paper can be used as supporting project context, but this README is intentionally grounded in repository-observable facts.
