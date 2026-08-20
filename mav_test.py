from pymavlink import mavutil
import os
import shutil
import time

PORT = "/dev/serial0"
BAUD = 460800

mav = mavutil.mavlink_connection(
    PORT,
    baud=BAUD,
    source_system=1,
    source_component=1,
)

boot_time = time.time()
last_status_text = 0
last_named_values = 0

lat = int(37.5665 * 1e7)
lon = int(126.9780 * 1e7)
alt_mm = 100000


def cpu_temp_c():
    try:
        with open("/sys/class/thermal/thermal_zone0/temp", "r", encoding="ascii") as f:
            return int(f.read().strip()) / 1000.0
    except OSError:
        return -1.0


def disk_free_mb(path="/"):
    usage = shutil.disk_usage(path)
    return int(usage.free / (1024 * 1024))


def send_named_float(name, value):
    mav.mav.named_value_float_send(
        int((time.time() - boot_time) * 1000),
        name.encode("ascii")[:10],
        float(value),
    )


def send_named_int(name, value):
    mav.mav.named_value_int_send(
        int((time.time() - boot_time) * 1000),
        name.encode("ascii")[:10],
        int(value),
    )

while True:
    now = time.time()
    now_ms = int((time.time() - boot_time) * 1000)

    mav.mav.heartbeat_send(
        mavutil.mavlink.MAV_TYPE_QUADROTOR,
        mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA,
        mavutil.mavlink.MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
        0,
        mavutil.mavlink.MAV_STATE_ACTIVE,
    )

    mav.mav.gps_raw_int_send(
        int(time.time() * 1000000),
        3,
        lat,
        lon,
        alt_mm,
        100,
        100,
        0,
        0,
        10,
    )

    mav.mav.global_position_int_send(
        now_ms,
        lat,
        lon,
        alt_mm,
        alt_mm,
        0,
        0,
        0,
        0,
    )

    if now - last_named_values >= 2:
        last_named_values = now
        send_named_float("cpu_temp", cpu_temp_c())
        send_named_int("sd_free", disk_free_mb("/"))
        send_named_int("uptime_s", int(now - boot_time))
        send_named_int("cam_count", 5)
        send_named_int("cam_state", 1)

    if now - last_status_text >= 10:
        last_status_text = now
        mav.mav.statustext_send(
            mavutil.mavlink.MAV_SEVERITY_INFO,
            b"CubeSat telemetry alive; cams=5; storage ok",
        )

    msg = mav.recv_match(blocking=False)
    if msg is not None:
        print(f"received from GCS: {msg.get_type()} {msg}", flush=True)

    print("sent MAVLink heartbeat/gps/status", flush=True)
    time.sleep(1)
