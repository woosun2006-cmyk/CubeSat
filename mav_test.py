#!/usr/bin/env python3
"""
Raspberry Pi -> UART -> XR1 (ELRS, Serial Protocol: MAVLink) -> Pocket (WiFi UDP) -> QGroundControl
Sends HEARTBEAT, a dummy GPS fix, and Pi health telemetry (cpu_temp / sd_free / uptime_s)
as NAMED_VALUE_FLOAT messages once per second.
"""

import os
import time

from pymavlink import mavutil

SERIAL_PORT = os.environ.get("MAVLINK_SERIAL_PORT", "/dev/serial0")
BAUD_RATE = int(os.environ.get("MAVLINK_BAUD", "57600"))
SEND_INTERVAL_S = 1.0

# Dummy test position (no GPS hardware attached yet)
TEST_LAT = 37.5665
TEST_LON = 126.9780
TEST_ALT_M = 50.0


def read_cpu_temp_c():
    with open("/sys/class/thermal/thermal_zone0/temp") as f:
        return int(f.read().strip()) / 1000.0


def read_sd_free_mb():
    st = os.statvfs("/")
    return (st.f_bavail * st.f_frsize) / (1024 * 1024)


def read_uptime_s():
    with open("/proc/uptime") as f:
        return float(f.read().split()[0])


def connect():
    master = mavutil.mavlink_connection(
        SERIAL_PORT,
        baud=BAUD_RATE,
        source_system=1,
        source_component=mavutil.mavlink.MAV_COMP_ID_ONBOARD_COMPUTER,
    )
    return master


def send_telemetry(master):
    time_usec = int(time.time() * 1e6)
    boot_ms = int(read_uptime_s() * 1000) & 0xFFFFFFFF

    master.mav.heartbeat_send(
        mavutil.mavlink.MAV_TYPE_GENERIC,
        mavutil.mavlink.MAV_AUTOPILOT_INVALID,
        0, 0,
        mavutil.mavlink.MAV_STATE_ACTIVE,
    )

    master.mav.gps_raw_int_send(
        time_usec,
        mavutil.mavlink.GPS_FIX_TYPE_3D_FIX,
        int(TEST_LAT * 1e7),
        int(TEST_LON * 1e7),
        int(TEST_ALT_M * 1000),
        65535, 65535,  # eph, epv unknown
        0,             # vel
        65535,         # cog unknown
        8,             # satellites_visible
    )

    master.mav.global_position_int_send(
        boot_ms,
        int(TEST_LAT * 1e7),
        int(TEST_LON * 1e7),
        int(TEST_ALT_M * 1000),
        int(TEST_ALT_M * 1000),
        0, 0, 0,       # vx, vy, vz
        0,             # hdg unknown
    )

    master.mav.named_value_float_send(time_usec // 1000 & 0xFFFFFFFF, b"cpu_temp", read_cpu_temp_c())
    master.mav.named_value_float_send(time_usec // 1000 & 0xFFFFFFFF, b"sd_free", read_sd_free_mb())
    master.mav.named_value_float_send(time_usec // 1000 & 0xFFFFFFFF, b"uptime_s", read_uptime_s())


def main():
    while True:
        try:
            master = connect()
            while True:
                send_telemetry(master)
                time.sleep(SEND_INTERVAL_S)
        except (OSError, mavutil.mavlink.MAVError) as e:
            print(f"serial link error: {e}, retrying in 3s")
            time.sleep(3)


if __name__ == "__main__":
    main()
