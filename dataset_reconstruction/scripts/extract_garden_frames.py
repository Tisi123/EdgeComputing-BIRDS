import shutil
import cv2
from pathlib import Path

VIDEO_DIR = Path("raw_videos")
OUT_DIR = Path("dataset/raw/neg_not_bird_garden")

SECONDS_BETWEEN_FRAMES = 4  # best value

if OUT_DIR.exists():
    shutil.rmtree(OUT_DIR)
OUT_DIR.mkdir(parents=True)


def extract_video(video_path):

    cap = cv2.VideoCapture(str(video_path))

    fps = cap.get(cv2.CAP_PROP_FPS)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    if fps == 0:
        fps = 30

    step = int(fps * SECONDS_BETWEEN_FRAMES)

    saved = 0
    frame_id = 0

    while True:

        ret, frame = cap.read()

        if not ret:
            break

        if frame_id % step == 0:

            outname = f"{video_path.stem}_{saved}.jpg"

            cv2.imwrite(
                str(OUT_DIR/outname),
                frame
            )

            saved += 1

        frame_id += 1

    cap.release()

    print(video_path.name, "->", saved)


videos = list(VIDEO_DIR.glob("*"))

total = 0

for v in videos:

    extract_video(v)

print("\nExtraction finished.")