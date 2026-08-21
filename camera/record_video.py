#!/usr/bin/env python3
"""Record a fixed-length 640x640 video from the Pi camera.

Usage:
    python3 record_video.py [seconds]

Saves an mp4 into this script's video/ subdirectory, named by timestamp.
"""
import os
import subprocess
import sys
import time
import yaml

WIDTH = HEIGHT = 640

SETTING_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "setting")
CAM_SETTINGS_PATH = os.path.join(SETTING_DIR, "cam_sets.yaml")

with open(CAM_SETTINGS_PATH, encoding="utf-8") as f:
    _cam_settings = yaml.safe_load(f)["record"]

CAMERA_ID = _cam_settings["camera_id"]
FPS = _cam_settings["fps"]
AWB_MODE = _cam_settings["awb_mode"]
QUALITY = _cam_settings["quality"]

VIDEO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "video")


def record(seconds):
    os.makedirs(VIDEO_DIR, exist_ok=True)
    name = f"video_{time.strftime('%m%d_%H%M%S')}.mp4"
    path = os.path.join(VIDEO_DIR, name)

    # No W:H sensor mode matches a 640x640 square, so --mode is left off:
    # rpicam-vid picks the closest native mode itself and the ISP
    # crops/scales it to the requested --width/--height output.
    cmd = [
        "rpicam-vid", "--camera", str(CAMERA_ID),
        "--width", str(WIDTH), "--height", str(HEIGHT),
        "--framerate", str(FPS),
        "--awb", AWB_MODE,
        "--codec", "libav", "--libav-format", "mp4",
        "--quality", str(QUALITY),
        "--nopreview", "--timeout", str(int(seconds * 1000)),
        "-o", path,
    ]
    print(f"recording {seconds:.0f}s at {WIDTH}x{HEIGHT} -> {path}", flush=True)
    subprocess.run(cmd, check=True, stderr=subprocess.DEVNULL)
    print(f"saved: {path}", flush=True)


if __name__ == "__main__":
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    record(secs)
