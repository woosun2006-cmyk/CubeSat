import os
import shutil
import sys
import time
import serial

from pymavlink import mavutil

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "setting"))
import conf

# 포트와 장치는 setting/port.yaml 에서 읽는다. 아래 값은 그 파일이 없을 때의
# 기본값일 뿐이므로, 배선을 바꾸면 코드가 아니라 port.yaml 을 고친다.
PORT = conf.get("elrs_device", "/dev/serial0")
BAUD = conf.num("elrs_baud", 460800)
PIXHAWK_PORT = conf.get("pixhawk_device", "/dev/ttyACM0")
PIXHAWK_BAUD = conf.num("pixhawk_baud", 115200)
# LoRa 는 gcs 와 같은 UART 를 쓴다. 세 링크를 한꺼번에 돌릴 때
# (scripts/gcs.sh)는 gcs 가 LoRa 를 맡으므로 이쪽은 MAV_LORA_DEVICE=off 로
# 꺼서 ELRS 만 보낸다. 환경변수가 없으면 예전대로 port.yaml 을 따른다.
LORA_PORT = os.environ.get("MAV_LORA_DEVICE") or conf.get(
    "lora_device", "/dev/ttyAMA3")
LORA_ENABLED = LORA_PORT not in ("off", "none", "")
LORA_BAUD = conf.num("lora_baud", 9600)
LORA_SEND_INTERVAL = conf.num("lora_send_interval", 1.0)
STORAGE_PATH = "/"
CAMERA_COUNT = 4
GPS_RAW_INT_MSG_ID = 24
GLOBAL_POSITION_INT_MSG_ID = 33
ATTITUDE_MSG_ID = 30
RAW_IMU_MSG_ID = 27
SCALED_IMU_MSG_ID = 26
HIGHRES_IMU_MSG_ID = 105
ATTITUDE_STALE_S = 3.0
IMU_STALE_S = 3.0

mav = mavutil.mavlink_connection(
    PORT,
    baud=BAUD,
    source_system=1,
    source_component=1,
)


def open_pixhawk():
    try:
        conn = mavutil.mavlink_connection(
            PIXHAWK_PORT,
            baud=PIXHAWK_BAUD,
            source_system=250,
            source_component=1,
        )
        print(f"Pixhawk GPS port {PIXHAWK_PORT} connected", flush=True)
        return conn
    except OSError as exc:
        print(f"warning: cannot open Pixhawk GPS port {PIXHAWK_PORT}: {exc}", flush=True)
        return None

def open_lora():
    if not LORA_ENABLED:
        return None

    try:
        conn = serial.Serial(
            LORA_PORT,
            LORA_BAUD,
            timeout=0.1,
        )
        print(
            f"LoRa port {LORA_PORT} connected @ {LORA_BAUD}",
            flush=True,
        )
        return conn

    except (OSError, serial.SerialException) as exc:
        print(
            f"warning: cannot open LoRa port {LORA_PORT}: {exc}",
            flush=True,
        )
        return None

pixhawk = open_pixhawk()
lora = open_lora()

if not LORA_ENABLED:
    print("LoRa 송신 꺼짐 (MAV_LORA_DEVICE=off). ELRS 만 보낸다.", flush=True)

# 메시지 종류별 송신 주기.
#
# 예전에는 루프 끝의 time.sleep(1) 하나가 전부여서 무엇이든 1 Hz 였다.
# ELRS 의 텔레메트리 다운링크는 수백 B/s 급이라 전부를 10 Hz 로 올리면
# 넘친다. 그래서 빨리 변하는 것(자세·위치)만 올리고, 느리게 변하거나
# 진단용인 것은 원래 주기에 둔다.
#
#   자세/위치   10 Hz  x ~80 B  = 800 B/s   <- 실제로 필요한 것
#   IMU          2 Hz  x ~34 B  =  68 B/s
#   GPS_RAW      1 Hz  x ~42 B  =  42 B/s   (위치는 위에서 이미 간다)
#   HEARTBEAT    1 Hz  x ~21 B  =  21 B/s
#   NAMED_VALUE 14개 / 10 s     =  36 B/s   (예전 2 s -> 10 s)
#   STATUSTEXT      / 10 s      =   6 B/s
#                               ~ 970 B/s
#
# 링크가 못 버티면 port.yaml 에서 elrs_send_hz 부터 낮출 것. 값을 코드가
# 아니라 설정에 둔 이유가 그것이다.
ELRS_SEND_HZ = conf.num("elrs_send_hz", 10.0)
ELRS_IMU_HZ = conf.num("elrs_imu_hz", 2.0)
ELRS_GPS_RAW_HZ = conf.num("elrs_gps_raw_hz", 1.0)
ELRS_HEARTBEAT_HZ = conf.num("elrs_heartbeat_hz", 1.0)
ELRS_NAMED_INTERVAL = conf.num("elrs_named_interval", 10.0)

_send_interval = 1.0 / ELRS_SEND_HZ if ELRS_SEND_HZ > 0 else 1.0
_imu_interval = 1.0 / ELRS_IMU_HZ if ELRS_IMU_HZ > 0 else 0.0
_gps_raw_interval = 1.0 / ELRS_GPS_RAW_HZ if ELRS_GPS_RAW_HZ > 0 else 0.0
_heartbeat_interval = 1.0 / ELRS_HEARTBEAT_HZ if ELRS_HEARTBEAT_HZ > 0 else 0.0

boot_time = time.time()
last_status_text = 0
last_named_values = 0
last_pixhawk_request = 0
last_lora_send = 0
last_heartbeat = 0
last_gps_raw = 0
last_imu_send = 0
last_console_line = 0
cycles = 0
lora_seq = 0

gps = {
    "fix_type": 0,
    "lat": 0,
    "lon": 0,
    "alt": 0,
    "eph": 9999,
    "epv": 9999,
    "vel": 0,
    "cog": 0,
    "satellites_visible": 0,
    "last_seen": 0,
}

# GLOBAL_POSITION_INT 에서 오는 값. 예전에는 전부 0 을 보냈는데, 그러면
# 지상국의 속도/수직속도/가속도/방위 칸이 링크와 무관하게 비어 있게 된다.
pos = {
    "rel_alt_mm": 0,
    "vx": 0,
    "vy": 0,
    "vz": 0,
    "hdg": 0,
    "last_seen": 0,
}

att = {
    "roll": 0.0,
    "pitch": 0.0,
    "yaw": 0.0,
    "rollspeed": 0.0,
    "pitchspeed": 0.0,
    "yawspeed": 0.0,
    "last_seen": 0,
}

imu = {
    "kind": "",
    "time_usec": 0,
    "time_boot_ms": 0,
    "xacc": 0,
    "yacc": 0,
    "zacc": 0,
    "xgyro": 0,
    "ygyro": 0,
    "zgyro": 0,
    "xmag": 0,
    "ymag": 0,
    "zmag": 0,
    "abs_pressure": 0.0,
    "diff_pressure": 0.0,
    "pressure_alt": 0.0,
    "temperature": 0.0,
    "fields_updated": 0,
    "last_seen": 0,
}


def cpu_temp_c():
    try:
        with open("/sys/class/thermal/thermal_zone0/temp", "r", encoding="ascii") as f:
            return int(f.read().strip()) / 1000.0
    except OSError:
        return -1.0


def disk_stats(path=STORAGE_PATH):
    usage = shutil.disk_usage(path)
    total_mb = int(usage.total / (1024 * 1024))
    used_mb = int(usage.used / (1024 * 1024))
    free_mb = int(usage.free / (1024 * 1024))
    used_percent = int((usage.used / usage.total) * 100) if usage.total else -1
    return total_mb, used_mb, free_mb, used_percent


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


def request_pixhawk_streams():
    if pixhawk is None:
        return

    if pixhawk.target_system == 0:
        pixhawk.wait_heartbeat(timeout=0.1)

    if pixhawk.target_system == 0:
        return

    for msg_id in (
        GPS_RAW_INT_MSG_ID,
        GLOBAL_POSITION_INT_MSG_ID,
        ATTITUDE_MSG_ID,
        RAW_IMU_MSG_ID,
        SCALED_IMU_MSG_ID,
        HIGHRES_IMU_MSG_ID,
    ):
        pixhawk.mav.command_long_send(
            pixhawk.target_system,
            pixhawk.target_component,
            mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL,
            0,
            msg_id,
            500000,
            0,
            0,
            0,
            0,
            0,
        )


def read_pixhawk():
    if pixhawk is None:
        return

    # pymavlink 2.4.49 의 add_message 는 인스턴스 필드(IMU 계열의 id)가 빈 채로
    # 먼저 들어온 타입을 그냥 덮어써 두고, 뒤에 그 필드가 채워진 같은 타입이
    # 오면 없는 캐시에 쓰려다 TypeError 로 죽는다. 픽스호크가 같은 IMU 를 v1 과
    # v2 로 섞어 보내면 그렇게 된다. 라이브러리를 고칠 자리가 아니므로 그
    # 메시지 하나만 버리고 계속 읽는다 -- 여기서 죽으면 ELRS 송신이 통째로
    # 멈춘다. 계속 터지면 이번 주기는 접고 다음 호출에서 다시 본다.
    dropped = 0

    while True:
        try:
            msg = pixhawk.recv_match(
                type=[
                    "GPS_RAW_INT",
                    "GLOBAL_POSITION_INT",
                    "ATTITUDE",
                    "RAW_IMU",
                    "SCALED_IMU",
                    "HIGHRES_IMU",
                ],
                blocking=False,
            )
        except TypeError:
            dropped += 1
            if dropped > 20:
                print(
                    "warning: pymavlink 인스턴스 캐시 오류가 이어진다. "
                    "이번 주기는 건너뛴다.",
                    flush=True,
                )
                break
            continue

        if msg is None:
            break

        if msg.get_type() == "GLOBAL_POSITION_INT":
            pos["rel_alt_mm"] = int(msg.relative_alt)
            pos["vx"] = int(msg.vx)
            pos["vy"] = int(msg.vy)
            pos["vz"] = int(msg.vz)
            pos["hdg"] = int(msg.hdg)
            pos["last_seen"] = time.time()
            continue

        if msg.get_type() == "ATTITUDE":
            att["roll"] = float(msg.roll)
            att["pitch"] = float(msg.pitch)
            att["yaw"] = float(msg.yaw)
            att["rollspeed"] = float(msg.rollspeed)
            att["pitchspeed"] = float(msg.pitchspeed)
            att["yawspeed"] = float(msg.yawspeed)
            att["last_seen"] = time.time()
            continue

        if msg.get_type() in ("RAW_IMU", "SCALED_IMU"):
            imu["kind"] = msg.get_type()
            imu["time_usec"] = int(getattr(msg, "time_usec", int(time.time() * 1000000)))
            imu["time_boot_ms"] = int(getattr(msg, "time_boot_ms", 0))
            imu["xacc"] = int(msg.xacc)
            imu["yacc"] = int(msg.yacc)
            imu["zacc"] = int(msg.zacc)
            imu["xgyro"] = int(msg.xgyro)
            imu["ygyro"] = int(msg.ygyro)
            imu["zgyro"] = int(msg.zgyro)
            imu["xmag"] = int(msg.xmag)
            imu["ymag"] = int(msg.ymag)
            imu["zmag"] = int(msg.zmag)
            imu["last_seen"] = time.time()
            continue

        if msg.get_type() == "HIGHRES_IMU":
            imu["kind"] = "HIGHRES_IMU"
            imu["time_usec"] = int(msg.time_usec)
            imu["xacc"] = float(msg.xacc)
            imu["yacc"] = float(msg.yacc)
            imu["zacc"] = float(msg.zacc)
            imu["xgyro"] = float(msg.xgyro)
            imu["ygyro"] = float(msg.ygyro)
            imu["zgyro"] = float(msg.zgyro)
            imu["xmag"] = float(msg.xmag)
            imu["ymag"] = float(msg.ymag)
            imu["zmag"] = float(msg.zmag)
            imu["abs_pressure"] = float(msg.abs_pressure)
            imu["diff_pressure"] = float(msg.diff_pressure)
            imu["pressure_alt"] = float(msg.pressure_alt)
            imu["temperature"] = float(msg.temperature)
            imu["fields_updated"] = int(msg.fields_updated)
            imu["last_seen"] = time.time()
            continue

        gps["fix_type"] = int(msg.fix_type)
        gps["lat"] = int(msg.lat) if msg.fix_type >= 2 else 0
        gps["lon"] = int(msg.lon) if msg.fix_type >= 2 else 0
        gps["alt"] = int(msg.alt) if msg.fix_type >= 2 else 0
        gps["eph"] = int(msg.eph)
        gps["epv"] = int(msg.epv)
        gps["vel"] = int(msg.vel)
        gps["cog"] = int(msg.cog)
        gps["satellites_visible"] = int(msg.satellites_visible)
        gps["last_seen"] = time.time()


def gps_is_valid():
    return gps["fix_type"] >= 2 and gps["lat"] != 0 and gps["lon"] != 0


def attitude_is_valid(now):
    # Pixhawk 가 조용해지면 마지막 자세값이 그대로 남는다. 오래된 값을
    # 유효한 것처럼 보내면 지상국이 멈춘 자세를 실시간인 척 그린다.
    return (
        att["last_seen"] != 0
        and now - att["last_seen"] < ATTITUDE_STALE_S
    )


def imu_is_valid(now):
    return (
        imu["last_seen"] != 0
        and now - imu["last_seen"] < IMU_STALE_S
    )


def send_imu(now, now_ms):
    if not imu_is_valid(now):
        return

    if imu["kind"] == "HIGHRES_IMU":
        mav.mav.highres_imu_send(
            imu["time_usec"] or int(time.time() * 1000000),
            imu["xacc"], imu["yacc"], imu["zacc"],
            imu["xgyro"], imu["ygyro"], imu["zgyro"],
            imu["xmag"], imu["ymag"], imu["zmag"],
            imu["abs_pressure"],
            imu["diff_pressure"],
            imu["pressure_alt"],
            imu["temperature"],
            imu["fields_updated"],
        )
        return

    if imu["kind"] == "SCALED_IMU":
        mav.mav.scaled_imu_send(
            imu["time_boot_ms"] or now_ms,
            imu["xacc"], imu["yacc"], imu["zacc"],
            imu["xgyro"], imu["ygyro"], imu["zgyro"],
            imu["xmag"], imu["ymag"], imu["zmag"],
        )
        return

    if imu["kind"] == "RAW_IMU":
        mav.mav.raw_imu_send(
            imu["time_usec"] or int(time.time() * 1000000),
            imu["xacc"], imu["yacc"], imu["zacc"],
            imu["xgyro"], imu["ygyro"], imu["zgyro"],
            imu["xmag"], imu["ymag"], imu["zmag"],
        )


def send_lora_telemetry(now):
    global lora
    global lora_seq

    if not LORA_ENABLED:
        return

    if lora is None:
        lora = open_lora()

        if lora is None:
            return

    lat_deg = gps["lat"] / 1e7 if gps["lat"] != 0 else 0.0
    lon_deg = gps["lon"] / 1e7 if gps["lon"] != 0 else 0.0

    alt_m = gps["alt"] / 1000.0
    vel_ms = gps["vel"] / 100.0
    cog_deg = gps["cog"] / 100.0

    timestamp_ms = int(now * 1000)

    payload = (
        f"SEQ={lora_seq},"
        f"TIME={timestamp_ms},"
        f"LAT={lat_deg:.7f},"
        f"LON={lon_deg:.7f},"
        f"ALT={alt_m:.2f},"
        f"FIX={gps['fix_type']},"
        f"SATS={gps['satellites_visible']},"
        f"VEL={vel_ms:.2f},"
        f"COG={cog_deg:.2f},"
        f"R={att['roll']:.4f},"
        f"P={att['pitch']:.4f},"
        f"Y={att['yaw']:.4f},"
        f"AV={1 if attitude_is_valid(now) else 0}\n"
    )

    try:
        lora.write(payload.encode("utf-8"))
        lora.flush()

        print(
            f"[LORA TX] {payload.strip()}",
            flush=True,
        )

        lora_seq += 1

    except (OSError, serial.SerialException) as exc:
        print(
            f"warning: LoRa TX failed: {exc}",
            flush=True,
        )

        try:
            lora.close()
        except Exception:
            pass

        lora = None

def gps_age_s(now):
    if gps["last_seen"] == 0:
        return -1
    return int(now - gps["last_seen"])


while True:
    now = time.time()
    now_ms = int((time.time() - boot_time) * 1000)

    if now - last_pixhawk_request >= 10:
        last_pixhawk_request = now
        if pixhawk is None:
            pixhawk = open_pixhawk()
        request_pixhawk_streams()

    read_pixhawk()
    gps_valid = gps_is_valid()

    if now - last_heartbeat >= _heartbeat_interval:
        last_heartbeat = now
        mav.mav.heartbeat_send(
            mavutil.mavlink.MAV_TYPE_QUADROTOR,
            mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA,
            mavutil.mavlink.MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
            0,
            mavutil.mavlink.MAV_STATE_ACTIVE,
        )

    # 위치 자체는 아래 GLOBAL_POSITION_INT 로 매 주기 나간다. 이쪽은
    # fix 종류/위성 수/정확도라 느리게 변하므로 주기를 따로 둔다.
    if now - last_gps_raw >= _gps_raw_interval:
        last_gps_raw = now
        mav.mav.gps_raw_int_send(
            int(time.time() * 1000000),
            gps["fix_type"],
            gps["lat"],
            gps["lon"],
            gps["alt"],
            gps["eph"],
            gps["epv"],
            gps["vel"],
            gps["cog"],
            gps["satellites_visible"],
        )

    mav.mav.global_position_int_send(
        now_ms,
        gps["lat"] if gps_valid else 0,
        gps["lon"] if gps_valid else 0,
        gps["alt"] if gps_valid else 0,
        # 아래 다섯은 픽스호크가 준 값을 그대로 넘긴다. 예전에는 0 을 보내서
        # 지상국의 상대고도/속도/수직속도/방위 칸이 항상 비어 있었다.
        pos["rel_alt_mm"],
        pos["vx"],
        pos["vy"],
        pos["vz"],
        pos["hdg"],
    )

    # ELRS 로도 자세를 내보낸다. att 는 read_pixhawk() 가 채운다.
    mav.mav.attitude_send(
        now_ms,
        att["roll"], att["pitch"], att["yaw"],
        att["rollspeed"], att["pitchspeed"], att["yawspeed"],
    )

    if now - last_imu_send >= _imu_interval:
        last_imu_send = now
        send_imu(now, now_ms)

    if now - last_named_values >= ELRS_NAMED_INTERVAL:
        last_named_values = now
        sd_total_mb, sd_used_mb, sd_free_mb, sd_used_percent = disk_stats()
        send_named_float("cpu_temp", cpu_temp_c())
        send_named_int("sd_free", sd_free_mb)
        send_named_int("sd_total", sd_total_mb)
        send_named_int("sd_used", sd_used_mb)
        send_named_int("sd_usedpct", sd_used_percent)
        send_named_int("uptime_s", int(now - boot_time))
        send_named_int("cam_count", CAMERA_COUNT)
        send_named_int("cam_state", 1)
        send_named_int("gps_fix", gps["fix_type"])
        send_named_int("gps_sats", gps["satellites_visible"])
        send_named_int("gps_valid", 1 if gps_valid else 0)
        send_named_int("gps_age_s", gps_age_s(now))
        send_named_int("att_valid", 1 if attitude_is_valid(now) else 0)
        send_named_int("imu_valid", 1 if imu_is_valid(now) else 0)

    if now - last_status_text >= 10:
        last_status_text = now
        _, _, sd_free_mb, _ = disk_stats()
        gps_status = "gps=fix" if gps_valid else f"gps=no_fix({gps['fix_type']})"
        status_text = f"cams={CAMERA_COUNT}; sd_free={sd_free_mb}MB; {gps_status}"
        mav.mav.statustext_send(
            mavutil.mavlink.MAV_SEVERITY_INFO,
            status_text.encode("ascii")[:50],
        )

    if (
        now - last_lora_send
        >= LORA_SEND_INTERVAL
    ):

        last_lora_send = now

        send_lora_telemetry(now)

    try:
        msg = mav.recv_match(blocking=False)
    except TypeError:
        # 위 read_pixhawk 와 같은 pymavlink 버그. 지상국에서 오는 쪽이다.
        msg = None
    if msg is not None:
        print(f"received from GCS: {msg.get_type()} {msg}", flush=True)

    # 화면은 1 초에 한 줄만. 10 Hz 로 찍으면 journal 이 흐르기만 하고
    # 읽을 수 없게 된다.
    cycles += 1
    if now - last_console_line >= 1:
        # 요청값이 아니라 실제로 돈 횟수를 찍는다. 링크나 파이가 못 따라가면
        # 여기서 바로 드러난다.
        measured_hz = cycles / (now - last_console_line)
        cycles = 0
        last_console_line = now
        print(
            f"sent MAVLink att/pos {measured_hz:.1f} Hz (목표 {ELRS_SEND_HZ:g}) "
            f"gps_fix={gps['fix_type']} gps_sats={gps['satellites_visible']} "
            f"att_valid={1 if attitude_is_valid(now) else 0} "
            f"imu_valid={1 if imu_is_valid(now) else 0} imu={imu['kind'] or 'none'}",
            flush=True,
        )

    # 다음 주기까지만 쉰다. 위 작업에 걸린 시간을 빼야 실제 주사율이
    # 요청한 값에 맞는다 -- sleep(고정값) 은 항상 그만큼 느려진다.
    sleep_for = _send_interval - (time.time() - now)
    if sleep_for > 0:
        time.sleep(sleep_for)
