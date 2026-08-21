#!/usr/bin/env python3
"""Always-on camera stream with a live fps graph and on-demand logging.

Start it once and leave it running. Open the printed URL: the video and the
fps chart are always live. Type how many seconds you want to measure, press
Record, move the camera around, and it writes a plain-text report into this
script's log/ subdirectory when the timer runs out.

Exposure is tracked alongside fps because auto-exposure is what usually drags
the frame rate down: a dark scene needs a longer shutter, and the shutter puts
a hard ceiling on fps (16.6ms -> 60fps, 33ms -> 30fps, 100ms -> 10fps).
"""
import os

os.environ.setdefault("OPENBLAS_CORETYPE", "ARMV8")

import http.server
import json
import re
import socket
import socketserver
import statistics
import subprocess
import threading
import time
import urllib.parse
from collections import deque

import cv2
import numpy as np
import yaml

SETTING_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "setting")
PORT_SETTINGS_PATH = os.path.join(SETTING_DIR, "port.yaml")
CAM_SETTINGS_PATH = os.path.join(SETTING_DIR, "cam_sets.yaml")

# ---------------------------------------------------------------------------
# SETTINGS - edit these
# ---------------------------------------------------------------------------
DEFAULT_RECORD_SEC = 30    # value pre-filled in the browser box
SAMPLE_INTERVAL_SEC = 0.5  # how often fps is measured
WARMUP_SKIP_SEC = 3.0      # ignored in the summary: auto-exposure has not
                           # settled yet and fps is computed from few frames
LIVE_WINDOW_SEC = 40       # how much history the idle chart shows

LOG_PREFIX = "fps_log"     # saved as <prefix>_MMDD_HHMM.txt
                           # e.g. fps_log_0721_2051.txt = Jul 21, 20:51

with open(PORT_SETTINGS_PATH, encoding="utf-8") as f:
    PORT = yaml.safe_load(f)["fpslog"]

with open(CAM_SETTINGS_PATH, encoding="utf-8") as f:
    _cam_yaml = yaml.safe_load(f)
    _cam_settings = _cam_yaml["fpslog"]
    _wb_correction = _cam_yaml["wb_correction"]

# Raspberry Pi 4 + Camera Module 2 (IMX219), captured via rpicam-vid
# (libcamera), not nvarguscamerasrc.
# IMX219 sensor modes on this board (from `rpicam-hello --list-cameras`):
#   0: 640x480   @200fps   1: 1640x1232 @81fps
#   2: 1920x1080 @47fps    3: 3280x2464 @21fps
# WIDTH/HEIGHT/FPS must match SENSOR_MODE exactly - rpicam-vid's --mode flag
# pins the sensor readout mode by W:H, it does not search for one that fits
# arbitrary caps.
CAMERA_ID = _cam_settings["camera_id"]
SENSOR_MODE = _cam_settings["sensor_mode"]
WIDTH, HEIGHT = _cam_settings["width"], _cam_settings["height"]
FPS = _cam_settings["fps"]             # requested fps; actual ceiling depends
                           # on the sensor mode (see table above)
FIXED_SHUTTER_US = _cam_settings["fixed_shutter_us"]  # 0 = auto exposure. Set
                           # e.g. 5000 (=5ms) to pin it and watch fps stay
                           # flat regardless of brightness. (rpicam-vid can
                           # report per-frame auto-exposure via --metadata,
                           # but this port keeps the simpler Jetson-style
                           # behaviour: exposure_us is only known/logged when
                           # fixed.)
AWB_MODE = _cam_settings["awb_mode"]  # auto/incandescent/tungsten/fluorescent/
                           # indoor/daylight/cloudy/custom. No white-balance
                           # cast has been measured on this module yet, so
                           # wb_correction below defaults to neutral (1.0) -
                           # tune it live via the color-correction sliders
                           # and hit Save once it looks right.
QUALITY = _cam_settings["quality"]  # rpicam-vid's mjpeg --quality (1-100,
                           # default 50); higher = bigger frames/more bitrate
# ---------------------------------------------------------------------------

# sensor_mode/width/height/fps always change together - a mode dropdown in
# the UI picks one of these presets rather than letting width/height/fps be
# set independently, since --mode needs them to match exactly.
SENSOR_MODES = [
    {"mode": 0, "width": 640, "height": 480, "fps": 200},
    {"mode": 1, "width": 1640, "height": 1232, "fps": 81},
    {"mode": 2, "width": 1920, "height": 1080, "fps": 47},
    {"mode": 3, "width": 3280, "height": 2464, "fps": 21},
]

FPS_WINDOW = 30  # frames averaged for one fps reading

state = {"frame": None, "times": deque(maxlen=FPS_WINDOW),
         "sizes": deque(maxlen=FPS_WINDOW), "count": 0, "proc": None}
cond = threading.Condition()
stopping = threading.Event()

# Live-tunable from the browser (mode/quality dropdown+slider) so cpu/gpu
# load can be watched while picking a resolution/bitrate. Changing either
# kills state["proc"], which makes grabber()'s read loop exit and rebuild
# the pipeline from these values - see grabber() and the /mode, /quality
# handlers below.
cam_lock = threading.Lock()
cam_state = {"sensor_mode": SENSOR_MODE, "width": WIDTH, "height": HEIGHT,
             "fps": FPS, "quality": QUALITY}
restart_requested = threading.Event()

# Live rgb correction preview, for visually tuning the wb_correction gains
# that camserver.py/yolo_live.py apply. Sliders are 0-255 with 128 =
# neutral (gain 1.0). Off by default: decoding+re-encoding every JPEG for
# the preview adds real overhead that would otherwise pollute this tool's
# own fps benchmark, so it only costs anything while actively tuning.
def _gain_to_slider(gain):
    return max(0, min(255, round(gain * 128)))

rgb_lock = threading.Lock()
rgb_state = {
    "enabled": False,
    "r": _gain_to_slider(_wb_correction["red_gain"]),
    "g": _gain_to_slider(_wb_correction["green_gain"]),
    "b": _gain_to_slider(_wb_correction["blue_gain"]),
}


def _save_wb_correction(red_gain, green_gain, blue_gain):
    """Rewrite just the three wb_correction values in cam_sets.yaml.

    A regex patch instead of yaml.safe_dump so the file's comments and
    section layout (camserver:/fpslog: settings, other people's edits)
    survive - a full re-dump would silently drop all of that.
    """
    with open(CAM_SETTINGS_PATH, encoding="utf-8") as f:
        text = f.read()
    for key, value in (("red_gain", red_gain), ("green_gain", green_gain),
                       ("blue_gain", blue_gain)):
        text = re.sub(rf"^(\s*{key}:\s*)[\d.]+", rf"\g<1>{value:.3f}",
                      text, count=1, flags=re.MULTILINE)
    with open(CAM_SETTINGS_PATH, "w", encoding="utf-8") as f:
        f.write(text)

history = deque(maxlen=int(LIVE_WINDOW_SEC / SAMPLE_INTERVAL_SEC) + 2)
hist_lock = threading.Lock()

rec = {"active": False, "rows": [], "t0": 0.0, "duration": 0.0,
       "saved": "", "started_str": ""}
rec_lock = threading.Lock()


def _apply_rgb_correction(jpeg_bytes):
    """Decode, scale channels by the live slider gains, re-encode.

    Only called while rgb_state["enabled"] - this round-trip is the cost of
    previewing the correction here, since the fast path otherwise never
    decodes a frame at all (see grabber()'s docstring).
    """
    with rgb_lock:
        r, g, b = rgb_state["r"], rgb_state["g"], rgb_state["b"]
    img = cv2.imdecode(np.frombuffer(jpeg_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return jpeg_bytes
    img = img.astype(np.float32)
    for i, gain in enumerate((b / 128.0, g / 128.0, r / 128.0)):  # BGR order
        if gain != 1.0:
            img[:, :, i] *= gain
    img = np.clip(img, 0, 255).astype(np.uint8)
    with cam_lock:
        quality = cam_state["quality"]
    ok, enc = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, quality])
    return enc.tobytes() if ok else jpeg_bytes


def grabber():
    """Feeds raw JPEG frames from rpicam-vid into state["frame"].

    rpicam-vid --codec mjpeg -o - writes back-to-back JPEGs with no
    container - same byte shape as the mjpeg stream this parser was
    written against, so the SOI/EOI scan below needs no changes. Frames
    pass through untouched unless rgb_state["enabled"] (see
    _apply_rgb_correction) - keeping this path free of decode/encode work
    is what makes the fps/bitrate numbers here trustworthy as a benchmark.
    """
    while not stopping.is_set():
        restart_requested.clear()
        with cam_lock:
            sensor_mode = cam_state["sensor_mode"]
            width, height, fps = cam_state["width"], cam_state["height"], cam_state["fps"]
            quality = cam_state["quality"]

        cmd = [
            "rpicam-vid", "--camera", str(CAMERA_ID),
            "--mode", f"{width}:{height}:10:P",
            "--width", str(width), "--height", str(height),
            "--framerate", str(fps),
            "--awb", AWB_MODE,
        ]
        if FIXED_SHUTTER_US > 0:
            cmd += ["--shutter", str(FIXED_SHUTTER_US)]
        cmd += [
            "--codec", "mjpeg", "--quality", str(quality),
            "--nopreview", "--timeout", "0",
            "-o", "-",
        ]

        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
        state["proc"] = proc
        buf = b""
        while not stopping.is_set():
            chunk = proc.stdout.read(8192)
            if not chunk:
                break
            buf += chunk
            while True:
                start = buf.find(b"\xff\xd8")
                end = buf.find(b"\xff\xd9", start + 2)
                if start == -1 or end == -1:
                    break
                frame = buf[start:end + 2]
                buf = buf[end + 2:]
                if rgb_state["enabled"]:
                    frame = _apply_rgb_correction(frame)
                now = time.monotonic()
                with cond:
                    state["frame"] = frame
                    state["times"].append(now)
                    state["sizes"].append(len(frame))
                    state["count"] += 1
                    cond.notify_all()
        proc.wait()
        if stopping.is_set():
            return
        if not restart_requested.is_set():
            time.sleep(3)  # crash/EOF backoff - skipped on a deliberate mode/quality change


def fixed_exposure_metadata():
    """exposure_us if we pinned it via FIXED_SHUTTER_US, else unknown.

    rpicam-vid can report per-frame auto-exposure via --metadata, but that
    stream isn't wired up here - only the fixed, self-imposed value is
    known/logged.
    """
    if FIXED_SHUTTER_US > 0:
        return {"exposure_us": str(FIXED_SHUTTER_US), "gain": "", "lux": ""}
    return {"exposure_us": "", "gain": "", "lux": ""}


_cpu_prev = {}
_thermal_zone_path = None


def _cpu_thermal_zone():
    """Path to the CPU thermal zone's temp file.

    thermal_zone0 happens to be "cpu-thermal" on this board, but look the
    zone up by name rather than assume the index stays that way.
    """
    global _thermal_zone_path
    if _thermal_zone_path:
        return _thermal_zone_path
    base = "/sys/class/thermal"
    try:
        for name in sorted(os.listdir(base)):
            if not name.startswith("thermal_zone"):
                continue
            with open(os.path.join(base, name, "type")) as f:
                if f.read().strip() == "cpu-thermal":
                    _thermal_zone_path = os.path.join(base, name, "temp")
                    return _thermal_zone_path
    except Exception:
        pass
    _thermal_zone_path = "/sys/class/thermal/thermal_zone0/temp"
    return _thermal_zone_path


def sysinfo():
    """CPU / RAM / clock / temperature of the Raspberry Pi itself."""
    out = {}
    try:
        with open("/proc/stat") as f:
            parts = [float(x) for x in f.readline().split()[1:8]]
        idle, total = parts[3] + parts[4], sum(parts)
        if _cpu_prev:
            dt, di = total - _cpu_prev["total"], idle - _cpu_prev["idle"]
            out["cpu_pct"] = round(100 * (dt - di) / dt, 1) if dt > 0 else 0.0
        _cpu_prev["total"], _cpu_prev["idle"] = total, idle
    except Exception:
        pass
    try:
        info = {}
        with open("/proc/meminfo") as f:
            for line in f:
                k, _, v = line.partition(":")
                info[k] = float(v.split()[0])  # kB
        tot, av = info.get("MemTotal", 0), info.get("MemAvailable", 0)
        if tot:
            out["mem_pct"] = round(100 * (tot - av) / tot, 1)
            out["mem_used_mb"] = round((tot - av) / 1024)
            out["mem_total_mb"] = round(tot / 1024)
    except Exception:
        pass
    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") as f:
            out["clock_mhz"] = round(int(f.read().strip()) / 1000)
    except Exception:
        pass
    try:
        with open(_cpu_thermal_zone()) as f:
            out["temp_c"] = round(int(f.read().strip()) / 1000, 1)
    except Exception:
        pass
    # No gpu_pct here: that was a Tegra-specific sysfs path (Jetson's
    # /sys/devices/57000000.gpu/load) with no Pi equivalent. cpu/clock/temp
    # above are what's actually available on this board.
    return out


def sample():
    with cond:
        times, sizes = list(state["times"]), list(state["sizes"])
        total = state["count"]
    fps = 0.0
    if len(times) >= 2:
        span = times[-1] - times[0]
        if span > 0:
            fps = (len(times) - 1) / span
    avg = sum(sizes) / len(sizes) if sizes else 0
    row = {"fps": round(fps, 2), "frame_kb": round(avg / 1024, 1),
           "mbps": round(fps * avg * 8 / 1_000_000, 2), "frames_total": total}
    row.update(fixed_exposure_metadata())
    row.update(sysinfo())
    return row


def sampler():
    """Runs for the whole life of the program. Feeds the live chart, and
    when a recording is armed, collects its rows and saves on completion."""
    while not stopping.is_set():
        row = sample()
        now = time.monotonic()
        with hist_lock:
            history.append((now, row))
        with rec_lock:
            if rec["active"]:
                elapsed = now - rec["t0"]
                r = dict(row)
                r["elapsed_s"] = round(elapsed, 2)
                rec["rows"].append(r)
                if elapsed >= rec["duration"]:
                    finish_locked()
        time.sleep(SAMPLE_INTERVAL_SEC)


def finish_locked():
    """Write the report. Caller must hold rec_lock."""
    rows, started_str = rec["rows"], rec["started_str"]
    duration = rec["duration"]
    rec["active"] = False
    if not rows:
        return
    name = f"{LOG_PREFIX}_{time.strftime('%m%d_%H%M')}.txt"
    log_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "log")
    os.makedirs(log_dir, exist_ok=True)
    path = os.path.join(log_dir, name)
    frames = 0
    try:
        frames = rows[-1]["frames_total"] - rows[0]["frames_total"]
    except Exception:
        pass
    with cam_lock:
        cam = dict(cam_state)
    text = report(rows, started_str, duration, frames, cam)
    with open(path, "w") as f:
        f.write(text + "\n")
    rec["saved"] = name
    print("\n" + text, flush=True)
    print(f"saved: {path}\n", flush=True)


# --- reporting --------------------------------------------------------------

def col(rows, key):
    out = []
    for r in rows:
        try:
            out.append(float(r.get(key, "")))
        except (TypeError, ValueError):
            pass
    return out


def describe(vals, unit="", scale=1.0):
    if not vals:
        return "n/a"
    v = [x * scale for x in vals]
    s = (f"mean {statistics.mean(v):7.2f}{unit}   "
         f"min {min(v):7.2f}{unit}   max {max(v):7.2f}{unit}")
    if len(v) > 1:
        s += f"   stdev {statistics.stdev(v):5.2f}"
    return s


def report(rows, started_str, duration, frames, cam):
    warm = [r for r in rows if r["elapsed_s"] < WARMUP_SKIP_SEC]
    body = [r for r in rows if r["elapsed_s"] >= WARMUP_SKIP_SEC] or rows
    fps = col(body, "fps")
    cam_fps = cam["fps"]

    W = 68
    L = ["=" * W, f" FPS LOG   {started_str}", "=" * W,
         f" requested      : {duration:.0f} s",
         f" resolution     : {cam['width']} x {cam['height']} "
         f"(sensor_mode {cam['sensor_mode']})",
         f" requested fps  : {cam_fps}",
         f" jpeg quality   : {cam['quality']}",
         " exposure       : " + (f"fixed {FIXED_SHUTTER_US/1000:.1f} ms"
                                 if FIXED_SHUTTER_US > 0 else "auto"),
         f" samples        : {len(rows)} "
         f"(first {len(warm)} skipped as warm-up)",
         f" frames         : {frames}", "",
         "-" * W, f" SUMMARY   (first {WARMUP_SKIP_SEC:.0f}s excluded)", "-" * W,
         f" fps       {describe(fps)}",
         f" exposure  {describe(col(body, 'exposure_us'), ' ms', 0.001)}",
         f" lux       {describe(col(body, 'lux'))}",
         f" bitrate   {describe(col(body, 'mbps'), ' Mbps')}",
         f" cpu       {describe(col(body, 'cpu_pct'), ' %')}",
         f" temp      {describe(col(body, 'temp_c'), ' C')}",
         f" clock     {describe(col(body, 'clock_mhz'), ' MHz')}", ""]

    if fps:
        drop = [r for r in body if r["fps"] < cam_fps * 0.9]
        L += ["-" * W, " VERDICT", "-" * W]
        if not drop:
            L.append(f" fps held near {cam_fps} for the whole run. No drops.")
        else:
            worst = min(drop, key=lambda r: r["fps"])
            L.append(f" {len(drop)} of {len(body)} samples fell below "
                     f"{cam_fps*0.9:.0f} fps.")
            L.append(f" worst {worst['fps']:.1f} fps at t={worst['elapsed_s']:.1f}s")
            if worst.get("exposure_us"):
                e = float(worst["exposure_us"]) / 1000
                budget = 1000.0 / cam_fps
                if e > budget:
                    L.append(f" exposure there was {e:.1f} ms > {budget:.1f} ms "
                             f"budget -> caps fps at about {1000/e:.0f}")
                    L.append(" the scene went dark, so auto-exposure lengthened")
                    L.append(" the shutter. set FIXED_SHUTTER_US to pin it.")
                else:
                    L.append(f" exposure was only {e:.1f} ms (fits the "
                             f"{budget:.1f} ms budget), so exposure is NOT")
                    L.append(" the cause. look at CPU load, disk I/O or network")
                    L.append(" instead - something stalled the capture loop.")
            else:
                L.append(" exposure is auto and not measurable on this")
                L.append(" platform (set FIXED_SHUTTER_US to pin & rule it")
                L.append(" out). look at CPU load, thermal or network below.")
        L.append("")

    L += ["-" * W, " FPS OVER TIME", "-" * W]
    top = max([cam_fps] + fps) if fps else cam_fps
    for r in rows:
        n = int(round(r["fps"] / top * 40)) if top else 0
        mark = " " if r["elapsed_s"] >= WARMUP_SKIP_SEC else "~"
        L.append(f"{mark}{r['elapsed_s']:6.1f}s {r['fps']:6.2f} "
                 f"|{'#'*n}{'.'*(40-n)}|")
    L += ["", " '~' = warm-up sample, excluded from the summary.", "=" * W]
    return "\n".join(L)


# --- web --------------------------------------------------------------------

PAGE = """<!doctype html><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>fps monitor</title>
<style>
 *{box-sizing:border-box}
 body{margin:0;padding:16px;background:#0d0f12;color:#e8eaed;
      font-family:system-ui,-apple-system,sans-serif;max-width:1400px}
 h1{font-size:14px;font-weight:600;color:#9aa0a6;margin:0 0 10px}
 .ctl{display:flex;gap:8px;align-items:center;flex-wrap:wrap;
      background:#16191d;border:1px solid #23272c;border-radius:8px;
      padding:10px 12px;margin-bottom:12px}
 .ctl label{font-size:12.5px;color:#9aa0a6}
 input{width:72px;background:#0d0f12;border:1px solid #3b4048;color:#e8eaed;
       border-radius:5px;padding:6px 8px;font:inherit;font-size:13px;
       font-variant-numeric:tabular-nums}
 button{background:#8ab4f8;color:#0d0f12;border:0;border-radius:5px;
        padding:7px 15px;font:inherit;font-size:13px;font-weight:600;
        cursor:pointer}
 button:hover{background:#a6c6fa}
 button.stop{background:#f28b82}
 button.stop:hover{background:#f5a29b}
 .st{font-size:12.5px;color:#6b7075;margin-left:auto}
 .bar{height:4px;background:#23272c;border-radius:2px;overflow:hidden;
      margin-bottom:14px}
 .bar i{display:block;height:100%;background:#8ab4f8;width:0;
        transition:width .4s linear}
 img{width:100%;border-radius:8px;display:block;background:#000;
     margin-bottom:14px}
 .row{display:grid;grid-template-columns:1fr 232px;gap:14px;align-items:start}
 @media(max-width:820px){.row{grid-template-columns:1fr}}
 canvas{width:100%;height:220px;background:#16191d;border:1px solid #23272c;
        border-radius:8px;display:block}
 .panel{background:#16191d;border:1px solid #23272c;border-radius:8px;
        padding:12px 14px}
 .panel h2{font-size:11px;font-weight:600;color:#6b7075;margin:0 0 9px;
           letter-spacing:.06em;text-transform:uppercase}
 .panel h2:not(:first-child){margin-top:15px;padding-top:13px;
                             border-top:1px solid #23272c}
 .r{display:flex;justify-content:space-between;align-items:baseline;
    padding:2.5px 0;font-size:12.5px}
 .r .k{color:#9aa0a6}
 .r .v{font-variant-numeric:tabular-nums;font-weight:600}
 .big{font-size:27px;font-weight:600;color:#8ab4f8;
      font-variant-numeric:tabular-nums;line-height:1.1}
 .big small{font-size:12px;color:#6b7075;font-weight:400;margin-left:3px}
 .warn{color:#f6c445} .bad{color:#f28b82} .ok{color:#81c995}
</style>
<body>
<h1>fps monitor</h1>

<div class=ctl>
  <label for=sec>measure for</label>
  <input id=sec type=number min=1 max=3600 value=__DEF__>
  <label>seconds</label>
  <button id=go>Record</button>
  <span class=st id=st>streaming</span>
</div>
<div class=ctl>
  <label for=mode>resolution mode</label>
  <select id=mode>__MODE_OPTIONS__</select>
  <label for=qual>quality</label>
  <input id=qual type=range min=1 max=100 value=__QUALITY__>
  <span id=qualval>__QUALITY__</span>
  <span class=st>restarts the capture pipeline - watch cpu/gpu below while tuning</span>
</div>
<div class=ctl>
  <label><input id=rgbon type=checkbox __RGB_CHECKED__> color correction</label>
  <label for=rr>r</label>
  <input id=rr type=range min=0 max=255 value=__R__><span id=rrval>__R__</span>
  <label for=rg>g</label>
  <input id=rg type=range min=0 max=255 value=__G__><span id=rgval>__G__</span>
  <label for=rb>b</label>
  <input id=rb type=range min=0 max=255 value=__B__><span id=rbval>__B__</span>
  <button id=rgbsave>Save</button>
  <span class=st id=rgbst>128 = neutral. adds decode/encode overhead while on</span>
</div>
<div class=bar><i id=prog></i></div>

<img src="/stream">

<div class=row>
  <canvas id=chart></canvas>
  <div class=panel>
    <h2>fps</h2>
    <div class=big><span id=fps>--</span><small>now</small></div>
    <div class=r><span class=k>average</span><span class=v id=avg>--</span></div>
    <div class=r><span class=k>min</span><span class=v id=mn>--</span></div>
    <div class=r><span class=k>max</span><span class=v id=mx>--</span></div>

    <h2>camera</h2>
    <div class=r><span class=k>resolution</span><span class=v id=res>--</span></div>
    <div class=r><span class=k>exposure</span><span class=v id=expo>--</span></div>
    <div class=r><span class=k>lux</span><span class=v id=lux>--</span></div>
    <div class=r><span class=k>bitrate</span><span class=v id=bw>--</span></div>

    <h2>raspberry pi</h2>
    <div class=r><span class=k>cpu</span><span class=v id=cpu>--</span></div>
    <div class=r><span class=k>ram</span><span class=v id=ram>--</span></div>
    <div class=r><span class=k>clock</span><span class=v id=clk>--</span></div>
    <div class=r><span class=k>temp</span><span class=v id=tmp>--</span></div>

    <h2>last saved</h2>
    <div class=r><span class=v id=saved style="font-size:11.5px">--</span></div>
  </div>
</div>

<script>
const cv=document.getElementById('chart'), cx=cv.getContext('2d');
let TOP=__FPS__;
const $=id=>document.getElementById(id);
let recording=false, xmax=__WIN__;

function draw(pts){
  const dpr=window.devicePixelRatio||1, W=cv.clientWidth, H=cv.clientHeight;
  cv.width=W*dpr; cv.height=H*dpr; cx.setTransform(dpr,0,0,dpr,0,0);
  cx.clearRect(0,0,W,H);
  const L=38,R=10,T=12,B=20, w=W-L-R, h=H-T-B, ymax=TOP*1.15;

  cx.strokeStyle='#23272c'; cx.fillStyle='#6b7075';
  cx.font='10px system-ui'; cx.lineWidth=1;
  for(let i=0;i<=4;i++){
    const v=ymax*i/4, y=T+h-(v/ymax)*h;
    cx.beginPath(); cx.moveTo(L,y); cx.lineTo(L+w,y); cx.stroke();
    cx.fillText(v.toFixed(0), 8, y+3);
  }
  cx.strokeStyle='#3b4048'; cx.setLineDash([3,3]);
  const ty=T+h-(TOP/ymax)*h;
  cx.beginPath(); cx.moveTo(L,ty); cx.lineTo(L+w,ty); cx.stroke();
  cx.setLineDash([]);
  cx.fillStyle='#6b7075'; cx.fillText('target '+TOP, L+w-52, ty-4);

  if(!pts.length) return;
  const xs=t=>L+(xmax?Math.min(t/xmax,1):0)*w;
  const ys=v=>T+h-(Math.min(v,ymax)/ymax)*h;
  const color=recording?'#8ab4f8':'#5f6976';

  cx.beginPath(); cx.moveTo(xs(pts[0].t), ys(pts[0].fps));
  pts.forEach(p=>cx.lineTo(xs(p.t), ys(p.fps)));
  cx.lineTo(xs(pts[pts.length-1].t), T+h); cx.lineTo(xs(pts[0].t), T+h);
  cx.closePath();
  const g=cx.createLinearGradient(0,T,0,T+h);
  g.addColorStop(0, recording?'rgba(138,180,248,.3)':'rgba(95,105,118,.22)');
  g.addColorStop(1,'rgba(0,0,0,0)');
  cx.fillStyle=g; cx.fill();

  cx.beginPath(); cx.moveTo(xs(pts[0].t), ys(pts[0].fps));
  pts.forEach(p=>cx.lineTo(xs(p.t), ys(p.fps)));
  cx.strokeStyle=color; cx.lineWidth=1.8; cx.stroke();

  cx.fillStyle='#6b7075';
  cx.fillText('0s', L, H-6);
  cx.fillText(xmax.toFixed(0)+'s', L+w-20, H-6);
}

$('go').onclick=async()=>{
  if(recording){ await fetch('/stop'); }
  else{
    const s=parseFloat($('sec').value)||30;
    await fetch('/start?sec='+s);
  }
  tick();
};

$('mode').onchange=async()=>{ await fetch('/mode?value='+$('mode').value); };
$('qual').oninput=()=>{ $('qualval').textContent=$('qual').value; };
$('qual').onchange=async()=>{ await fetch('/quality?value='+$('qual').value); };

$('rgbon').onchange=async()=>{
  await fetch('/rgb?enabled='+($('rgbon').checked?'1':'0'));
};
const rgbSlider=(id,val,label)=>{
  $(id).oninput=()=>{ $(label).textContent=$(id).value; };
  $(id).onchange=async()=>{ await fetch('/rgb?'+val+'='+$(id).value); };
};
rgbSlider('rr','r','rrval'); rgbSlider('rg','g','rgval'); rgbSlider('rb','b','rbval');
$('rgbsave').onclick=async()=>{
  const r=await (await fetch('/rgb_save')).json();
  $('rgbst').textContent='saved: r='+r.red_gain+' g='+r.green_gain+' b='+r.blue_gain;
};

async function tick(){
  try{
    const s=await (await fetch('/data')).json();
    const c=s.current||{};
    recording=s.recording; xmax=s.xmax||__WIN__;
    TOP=s.fps_target||TOP;

    $('fps').textContent=(c.fps??0).toFixed(1);
    $('avg').textContent=s.avg!=null?s.avg.toFixed(1):'--';
    $('mn').textContent =s.min!=null?s.min.toFixed(1):'--';
    $('mx').textContent =s.max!=null?s.max.toFixed(1):'--';

    $('res').textContent=s.width+'x'+s.height;
    const e=parseFloat(c.exposure_us||0)/1000;
    $('expo').textContent=e?e.toFixed(1)+' ms':'--';
    $('expo').className='v'+(e>1000/TOP?' warn':'');
    $('lux').textContent=c.lux?parseFloat(c.lux).toFixed(0):'--';
    $('bw').textContent=(c.mbps??0).toFixed(1)+' Mbps';

    const cpu=c.cpu_pct;
    $('cpu').textContent=cpu!=null?cpu.toFixed(0)+' %':'--';
    $('cpu').className='v'+(cpu>85?' bad':cpu>60?' warn':'');
    $('ram').textContent=c.mem_used_mb!=null
      ? c.mem_used_mb+' / '+c.mem_total_mb+' MB':'--';
    $('clk').textContent=c.clock_mhz?c.clock_mhz+' MHz':'--';
    const t=c.temp_c;
    $('tmp').textContent=t!=null?t.toFixed(1)+' C':'--';
    $('tmp').className='v'+(t>75?' bad':t>65?' warn':' ok');
    $('saved').textContent=s.saved||'--';

    $('go').textContent=recording?'Stop':'Record';
    $('go').className=recording?'stop':'';
    $('sec').disabled=recording;
    $('st').textContent=recording
      ? 'recording '+s.elapsed.toFixed(0)+' / '+s.duration.toFixed(0)+'s'
      : 'streaming (idle)';
    $('prog').style.width=recording
      ? Math.min(100,(s.elapsed/s.duration)*100)+'%' : '0%';

    draw(s.points||[]);
  }catch(err){}
}
setInterval(tick, 500); tick();
window.addEventListener('resize', ()=>tick());
</script>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj):
        self._send(json.dumps(obj).encode(), "application/json")

    def do_GET(self):
        path, _, query = self.path.partition("?")
        q = urllib.parse.parse_qs(query)

        if path == "/":
            with cam_lock:
                cur_mode, cur_quality = cam_state["sensor_mode"], cam_state["quality"]
            with rgb_lock:
                rgb = dict(rgb_state)
            options = "".join(
                f'<option value={m["mode"]}'
                f'{" selected" if m["mode"] == cur_mode else ""}>'
                f'{m["mode"]}: {m["width"]}x{m["height"]} @{m["fps"]}fps</option>'
                for m in SENSOR_MODES
            )
            page = (PAGE.replace("__DEF__", str(DEFAULT_RECORD_SEC))
                        .replace("__FPS__", str(FPS))
                        .replace("__WIN__", str(LIVE_WINDOW_SEC))
                        .replace("__MODE_OPTIONS__", options)
                        .replace("__QUALITY__", str(cur_quality))
                        .replace("__R__", str(rgb["r"]))
                        .replace("__G__", str(rgb["g"]))
                        .replace("__B__", str(rgb["b"]))
                        .replace("__RGB_CHECKED__", "checked" if rgb["enabled"] else ""))
            return self._send(page.encode(), "text/html; charset=utf-8")

        if path == "/start":
            try:
                sec = max(1.0, min(3600.0, float(q.get("sec", ["30"])[0])))
            except ValueError:
                sec = float(DEFAULT_RECORD_SEC)
            with rec_lock:
                rec.update(active=True, rows=[], t0=time.monotonic(),
                           duration=sec, saved="",
                           started_str=time.strftime("%Y-%m-%d %H:%M:%S"))
            print(f"recording {sec:.0f}s...", flush=True)
            return self._json({"ok": True, "duration": sec})

        if path == "/stop":
            with rec_lock:
                if rec["active"]:
                    finish_locked()
            return self._json({"ok": True})

        if path == "/data":
            return self._json(self._data())

        if path == "/mode":
            try:
                idx = int(q.get("value", [""])[0])
                preset = next(m for m in SENSOR_MODES if m["mode"] == idx)
            except (StopIteration, ValueError, IndexError):
                return self._json({"ok": False})
            with cam_lock:
                cam_state.update(sensor_mode=preset["mode"], width=preset["width"],
                                  height=preset["height"], fps=preset["fps"])
                proc = state["proc"]
            restart_requested.set()
            if proc:
                proc.terminate()
            return self._json({"ok": True, **preset})

        if path == "/quality":
            try:
                val = max(1, min(100, int(q.get("value", [""])[0])))
            except (ValueError, IndexError):
                return self._json({"ok": False})
            with cam_lock:
                cam_state["quality"] = val
                proc = state["proc"]
            restart_requested.set()
            if proc:
                proc.terminate()
            return self._json({"ok": True, "quality": val})

        if path == "/rgb":
            try:
                with rgb_lock:
                    if "r" in q:
                        rgb_state["r"] = max(0, min(255, int(q["r"][0])))
                    if "g" in q:
                        rgb_state["g"] = max(0, min(255, int(q["g"][0])))
                    if "b" in q:
                        rgb_state["b"] = max(0, min(255, int(q["b"][0])))
                    if "enabled" in q:
                        rgb_state["enabled"] = q["enabled"][0] == "1"
                    out = dict(rgb_state)
            except (ValueError, IndexError):
                return self._json({"ok": False})
            return self._json({"ok": True, **out})

        if path == "/rgb_save":
            with rgb_lock:
                r, g, b = rgb_state["r"], rgb_state["g"], rgb_state["b"]
            gains = {"red_gain": r / 128.0, "green_gain": g / 128.0, "blue_gain": b / 128.0}
            _save_wb_correction(**gains)
            print(f"saved wb_correction: {gains}", flush=True)
            return self._json({"ok": True, **{k: round(v, 3) for k, v in gains.items()}})

        if path == "/stream":
            return self.stream()

        self.send_error(404)

    def _data(self):
        with rec_lock:
            active, rows = rec["active"], list(rec["rows"])
            duration, saved = rec["duration"], rec["saved"]
            t0 = rec["t0"]
        with hist_lock:
            hist = list(history)

        if active and rows:
            pts = [{"t": r["elapsed_s"], "fps": r["fps"]} for r in rows]
            xmax = duration
            pool = [r["fps"] for r in rows
                    if r["elapsed_s"] >= WARMUP_SKIP_SEC and r["fps"] > 0]
            elapsed = time.monotonic() - t0
        else:
            base = hist[0][0] if hist else 0
            pts = [{"t": round(t - base, 2), "fps": r["fps"]} for t, r in hist]
            xmax = LIVE_WINDOW_SEC
            pool = [r["fps"] for _, r in hist if r["fps"] > 0]
            elapsed = 0.0

        cur = (rows[-1] if active and rows
               else (hist[-1][1] if hist else {}))
        with cam_lock:
            cam = dict(cam_state)
        with rgb_lock:
            rgb = dict(rgb_state)
        out = {"points": pts, "current": cur, "width": cam["width"],
               "height": cam["height"], "fps_target": cam["fps"],
               "sensor_mode": cam["sensor_mode"], "quality": cam["quality"],
               "rgb": rgb,
               "recording": active, "elapsed": round(elapsed, 1),
               "duration": duration, "xmax": xmax, "saved": saved}
        if pool:
            out["avg"] = round(sum(pool) / len(pool), 2)
            out["min"] = min(pool)
            out["max"] = max(pool)
        return out

    def stream(self):
        self.send_response(200)
        self.send_header("Content-Type",
                         "multipart/x-mixed-replace; boundary=FRAME")
        self.end_headers()
        last = None
        try:
            while not stopping.is_set():
                with cond:
                    while state["frame"] is last or state["frame"] is None:
                        cond.wait(timeout=1)
                        if stopping.is_set():
                            return
                    frame = state["frame"]
                last = frame
                self.wfile.write(b"--FRAME\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode())
                self.wfile.write(frame)
                self.wfile.write(b"\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    threading.Thread(target=grabber, daemon=True).start()

    print("warming up...", flush=True)
    deadline = time.monotonic() + 12
    while state["count"] < 5 and time.monotonic() < deadline:
        time.sleep(0.2)
    if state["count"] == 0:
        print("no frames from the camera - is it connected?", flush=True)
        stopping.set()
        return

    threading.Thread(target=sampler, daemon=True).start()

    httpd = Server(("0.0.0.0", PORT), Handler)
    # Lead with the plain IP: .local (mDNS) often fails to resolve on
    # Windows/Android clients even when avahi-daemon is running fine here.
    print(f"open this on your phone/PC : http://{lan_ip()}:{PORT}/")
    print(f"(mDNS alt, may not resolve): http://{socket.gethostname()}.local:{PORT}/")
    print("streaming. set the seconds in the browser and press Record.",
          flush=True)
    print("ctrl-c to quit.", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping...", flush=True)
    finally:
        stopping.set()
        with rec_lock:
            if rec["active"]:
                finish_locked()
        proc = state.get("proc")
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        httpd.server_close()
        print("camera released. bye.", flush=True)


if __name__ == "__main__":
    main()
