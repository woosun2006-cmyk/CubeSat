#!/bin/sh
set -eu

# 세 링크로 한꺼번에 내보낸다. LTE(UDP/VPN) + LoRa(E220) + ELRS(CRSF).
#
#   ./gcs.sh          세 링크 + 파이 GUI (기본)
#   ./gcs.sh 10       위와 같되 LTE 를 10 Hz 로 (LoRa/ELRS 는 자기 주기 유지)
#   ./gcs.sh nogui    GUI 없이. ssh 로 띄워두고 나갈 때.
#   ./gcs.sh build    gcs 와 gui 바이너리 빌드
#   ./gcs.sh status   설정 확인
#
# gcs_LTE.sh / gcs_LoRa.sh / gcs_ELRS.sh 를 세 개 띄우는 것이 아니다. 셋은 같은
# 장치를 두고 서로를 밀어내도록 만들어져 있다 -- LTE 와 LoRa 는 같은 픽스호크
# 포트를, LoRa 와 ELRS 는 같은 ttyAMA3 을 연다. 그래서 여기서는 링크가 겹치지
# 않게 프로그램 두 개로 나눠 띄운다.
#
#   Pixhawk -if02 -> gcs   -+-> tee.py -+-> GUI (127.0.0.1)
#                           |           +-> 노트북 (UDP/VPN)  ... LTE
#                           +-> /dev/ttyAMA3 (E220)           ... LoRa
#   Pixhawk -if00 -> mav.py --> /dev/serial0 -> ELRS TX 백팩   ... ELRS
#
# gcs 와 mav.py 는 픽스호크가 내놓는 서로 다른 USB 포트를 쓰므로 같이 돌 수
# 있다. 겹치는 것은 LoRa UART 하나뿐이고 그것은 gcs 가 맡는다. mav.py 는
# MAV_LORA_DEVICE=off 로 띄워 ELRS 만 보내게 한다.
#
# LTE 와 LoRa 는 같은 gcs 가 만든 같은 JSON 한 줄이므로 seq 를 맞춰 보면 두
# 링크의 손실률이 그대로 나온다. ELRS 는 MAVLink 메시지 자체를 나르는 다른
# 계통이라 그 비교에는 들어가지 않는다.
#
# 링크마다 속도가 다르다. 인자로 주는 주사율은 LTE(와 온보드 로그)에만 걸리고,
# LoRa 는 port.yaml 의 lora_send_interval, ELRS 는 mav.py 자기 루프를 따른다.
# gcs.sh 10 이면 LTE 만 10 Hz 로 오르고 나머지 둘은 그대로다. E220 은 한 줄에
# 0.87 초가 걸려 따라올 수 없고, ELRS 는 MAVLink 스트림이라 계통이 다르다.
#
# 두 서비스(gcs, mavlink)가 같은 장치를 쥐고 있으므로 도는 동안 멈추고 끝나면
# 원래 상태로 되돌린다. sudo 가 필요하니 ssh -t 로 붙거나 미리 sudo -v 를 해
# 둘 것.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
# conf.sh 는 소스하기 전에 PROJECT_DIR 이 정해져 있어야 한다.
PROJECT_DIR="$ROOT/gcs"
ELRS_DIR="$ROOT/ELRS"
. "$PROJECT_DIR/conf.sh"

# 사람마다 다른 값(노트북 주소, 포트)은 conf.sh 가 여는 connecting_port.yaml 에,
# 기체 장치는 setting/port.yaml 에 있다. 뒤쪽은 CONF_FILE 을 잠깐 바꿔 읽는다.
PORT_CONF=${PORT_CONF:-$ROOT/setting/port.yaml}
pconf() {
    _saved_conf=$CONF_FILE
    CONF_FILE=$PORT_CONF
    _pvalue=$(conf "$1" "$2")
    CONF_FILE=$_saved_conf
    printf '%s\n' "$_pvalue"
}

BIN="$PROJECT_DIR/gui"
GCS_BIN="$PROJECT_DIR/gcs"
GCS_SCRIPT="$PROJECT_DIR/gcs.sh"
TEE_SCRIPT="$PROJECT_DIR/tee.py"
PY=${PY:-$ROOT/mavenv/bin/python}

PORT=${GUI_PORT:-$(conf telemetry_port 14550)}
TEE_PORT=${TEE_PORT:-$(conf tee_port 14553)}
LAPTOP="$(conf gcs_host 10.0.0.16):$(conf telemetry_port 14550)"
LORA_DEVICE=${GCS_LORA_DEVICE:-$(pconf lora_device /dev/ttyAMA3)}
LORA_BAUD=${GCS_LORA_BAUD:-$(pconf lora_baud 9600)}
ELRS_DEVICE=$(pconf elrs_device /dev/serial0)
ELRS_BAUD=$(pconf elrs_baud 460800)
GCS_LOG=${GCS_LOG:-/tmp/cubesat-gcs-3link.log}
TEE_LOG=${TEE_LOG:-/tmp/cubesat-tee.log}
ELRS_LOG=${ELRS_LOG:-/tmp/cubesat-elrs.log}

MODE=${1:-run}
SEND_HZ=${GCS_SEND_HZ:-$(conf send_hz 1)}

# 숫자 인자는 주사율. gcs.sh 에는 환경변수로 넘어간다.
if is_hz "${1:-}"; then
    SEND_HZ=$1
    MODE=run
elif is_hz "${2:-}"; then
    SEND_HZ=$2
fi
export GCS_SEND_HZ=$SEND_HZ

# LoRa 는 gcs 가 맡는다.
export GCS_LORA_DEVICE=$LORA_DEVICE
export GCS_LORA_BAUD=$LORA_BAUD

# LoRa 는 자기 주기로 나간다. port.yaml 은 주기(초)로 적어 두므로 뒤집는다.
# 인자로 준 주사율은 LTE(UDP)와 온보드 로그에만 걸린다 -- E220 은 한 줄
# 내보내는 데만 0.87 초가 들어 따라올 수 없기 때문이다.
LORA_HZ=${GCS_LORA_HZ:-$(awk -v v="$(pconf lora_send_interval 1)" \
    'BEGIN { printf "%g", (v > 0 ? 1 / v : 1) }')}
export GCS_LORA_HZ=$LORA_HZ


build_gui() {
    gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
        "$PROJECT_DIR/GUI.c" -lncurses -o "$BIN"
}

# mavlink.service 가 MAV_LORA_DEVICE=off 로 도는 동안에는 ELRS 만 맡고 LoRa
# UART 는 비어 있다. 그러면 서비스를 멈출 이유가 없고, 따라서 sudo 도 필요
# 없다 -- 노트북에서 ssh 로 띄울 때 비밀번호를 물어볼 화면이 없기 때문에 이
# 구분이 중요하다. 예전처럼 LoRa 까지 쓰는 설정이면 그때만 잠시 넘겨받는다.
service_is_elrs_only() {
    systemctl show mavlink -p Environment 2>/dev/null \
        | grep -q 'MAV_LORA_DEVICE=off'
}

# 비밀번호 없이 되면 그대로 쓰고, 안 되면 한 번 물어본다.
svc() {
    if sudo -n systemctl "$1" "$2" 2>/dev/null; then
        return 0
    fi
    sudo systemctl "$1" "$2"
}

MAVLINK_WAS_ACTIVE=0
SERVICE_WAS_ACTIVE=0
ELRS_BY_SERVICE=0
GCS_PID=
TEE_PID=
MAV_PID=
CLEANED=0

cleanup() {
    # Ctrl-C 는 INT 와 EXIT 를 잇달아 때린다. 서비스를 두 번 올리지 않는다.
    if [ "$CLEANED" -eq 1 ]; then
        return 0
    fi
    CLEANED=1
    for _pid in "$TEE_PID" "$GCS_PID" "$MAV_PID"; do
        [ -n "$_pid" ] && kill "$_pid" 2>/dev/null || true
    done
    for _pid in "$TEE_PID" "$GCS_PID" "$MAV_PID"; do
        [ -n "$_pid" ] && wait "$_pid" 2>/dev/null || true
    done
    if [ "$SERVICE_WAS_ACTIVE" -eq 1 ]; then
        echo "gcs: gcs.service 를 다시 시작한다."
        svc start gcs || \
            echo "gcs: 자동 복구 실패. sudo systemctl start gcs 를 직접 실행할 것." >&2
    fi
    if [ "$MAVLINK_WAS_ACTIVE" -eq 1 ]; then
        echo "gcs: mavlink.service 를 다시 시작한다."
        svc start mavlink || \
            echo "gcs: 자동 복구 실패. sudo systemctl start mavlink 를 직접 실행할 것." >&2
    fi
}

case "$MODE" in
    build)
        "$GCS_SCRIPT" build
        build_gui
        echo "gcs: built $GCS_BIN, $BIN"
        exit 0
        ;;
    status)
        echo "설정 파일: $CONF_FILE"
        echo "장치 파일: $PORT_CONF"
        echo "활성 지상국: $(conf active -)"
        echo "LTE  : $LAPTOP  (gcs -> UDP)"
        echo "LoRa : $LORA_DEVICE @ $LORA_BAUD  (gcs -> E220)"
        echo "ELRS : $ELRS_DEVICE @ $ELRS_BAUD  (mav.py -> 백팩)"
        echo "픽스호크: $(conf pixhawk_device -) (gcs), $(pconf pixhawk_device /dev/ttyACM0) (mav.py)"
        echo "GUI 수신 포트: $PORT / 중계 포트: $TEE_PORT"
        echo "송신 속도: LTE $SEND_HZ Hz / LoRa $LORA_HZ Hz / ELRS mav.py 자체 주기"
        echo "mavlink.service: $(systemctl is-active mavlink 2>/dev/null || true)"
        echo "gcs.service: $(systemctl is-active gcs 2>/dev/null || true)"
        for _f in "$GCS_BIN" "$BIN" "$PY" "$ELRS_DIR/mav.py"; do
            if [ -x "$_f" ] || [ -f "$_f" ]; then
                echo "있음: $_f"
            else
                echo "없음: $_f"
            fi
        done
        exit 0
        ;;
    run|nogui) ;;
    *)
        echo "usage: $0 {run|nogui|build|status} [hz]" >&2
        echo "       $0 <hz>" >&2
        exit 2
        ;;
esac

# GUI 는 ncurses 라 터미널이 있어야 뜬다. ssh 로 명령만 던지면 여기서 걸러야
# 서비스를 멈췄다 되돌리는 헛수고를 안 한다.
if [ "$MODE" != "nogui" ] && [ ! -t 1 ]; then
    echo "gcs: 터미널이 없어 GUI 를 못 띄운다. nogui 로 실행하거나 ssh -t 로 붙을 것." >&2
    exit 2
fi


# LoRa 병목은 UART 가 아니라 E220 의 무선 속도다. 한 줄 262 B, 기본 에어레이트
# 2.4 kbps 면 한 줄에 0.87 초가 든다. LTE 주사율은 LoRa 를 끌고 가지 않으므로
# 여기서 보는 것은 LoRa 자기 주기뿐이다.
if ! awk -v v="$LORA_HZ" 'BEGIN { exit !(v <= 1) }'; then
    echo "gcs: 경고 - LoRa $LORA_HZ Hz 는 과하다. 한 줄 262 B, 2.4 kbps 면 1 Hz 가 한계." >&2
fi

if [ ! -c "$LORA_DEVICE" ]; then
    echo "gcs: LoRa 장치 $LORA_DEVICE 가 없다. config.txt 의 dtoverlay=uart3 확인할 것." >&2
    exit 1
fi
if [ ! -e "$ELRS_DEVICE" ]; then
    echo "gcs: ELRS 장치 $ELRS_DEVICE 가 없다." >&2
    exit 1
fi
if [ ! -x "$PY" ]; then
    echo "gcs: $PY 가 없다. README 의 venv 구성을 먼저 할 것." >&2
    exit 1
fi

[ -x "$GCS_BIN" ] || "$GCS_SCRIPT" build
if [ "$MODE" != "nogui" ]; then
    [ -x "$BIN" ] || build_gui
fi

# ELRS 는 서비스가 이미 맡고 있으면 그대로 둔다. 그 편이 sudo 없이 돌고,
# 이 스크립트를 껐다 켜도 ELRS 송신이 끊기지 않는다.
if systemctl is-active --quiet mavlink 2>/dev/null; then
    if service_is_elrs_only; then
        ELRS_BY_SERVICE=1
        echo "gcs: ELRS - mavlink.service 가 맡고 있다 (LoRa 는 꺼짐). 그대로 둔다."
    else
        echo "gcs: mavlink.service 가 LoRa 까지 쓰고 있다. 잠시 멈춘다 (종료 시 복구)."
        if ! svc stop mavlink; then
            echo "gcs: 서비스를 멈추지 못했다. sudo systemctl stop mavlink 후 다시 실행할 것." >&2
            exit 1
        fi
        MAVLINK_WAS_ACTIVE=1
        sleep 1
    fi
fi
if systemctl is-active --quiet gcs 2>/dev/null; then
    echo "gcs: gcs.service 를 잠시 멈춘다 (종료 시 자동 복구)."
    if ! svc stop gcs; then
        echo "gcs: 서비스를 멈추지 못했다. sudo systemctl stop gcs 후 다시 실행할 것." >&2
        exit 1
    fi
    SERVICE_WAS_ACTIVE=1
    sleep 1
fi
trap cleanup EXIT INT TERM

# 서비스를 막 멈춘 직후에는 그 파이썬이 아직 정리 중일 수 있다. 사라지기를
# 몇 초 기다려 보고 그래도 남아 있을 때만 사람 손을 탄 것으로 본다.
if [ "$ELRS_BY_SERVICE" -eq 0 ]; then
    waited=0
    while pgrep -f "python.*mav[.]py" >/dev/null 2>&1; do
        if [ "$waited" -ge 5 ]; then
            echo "gcs: 손으로 띄운 mav.py 가 돌고 있다. 먼저 종료할 것." >&2
            exit 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
fi
if pgrep -x gcs >/dev/null 2>&1; then
    echo "gcs: 이미 실행 중인 gcs 가 있다. 먼저 종료할 것." >&2
    exit 1
fi

# ELRS. 서비스가 맡고 있으면 그쪽에 맡기고, 아니면 여기서 띄운다.
# 어느 쪽이든 LoRa 는 gcs 가 쓰므로 mav.py 의 LoRa 는 꺼 둔다.
if [ "$ELRS_BY_SERVICE" -eq 0 ]; then
    echo "gcs: ELRS - mav.py -> $ELRS_DEVICE @ $ELRS_BAUD (로그: $ELRS_LOG)"
    MAV_LORA_DEVICE=off "$PY" -u "$ELRS_DIR/mav.py" >"$ELRS_LOG" 2>&1 &
    MAV_PID=$!
    sleep 2
    if ! kill -0 "$MAV_PID" 2>/dev/null; then
        MAV_PID=
        echo "gcs: 경고 - mav.py 가 바로 죽었다. ELRS 없이 계속한다. $ELRS_LOG 를 볼 것." >&2
    fi
fi

echo "gcs: LTE - $LAPTOP ($SEND_HZ Hz) / LoRa - $LORA_DEVICE @ $LORA_BAUD ($LORA_HZ Hz)"

if [ "$MODE" = "nogui" ]; then
    echo "gcs: GUI 없이 돈다. 멈추려면 Ctrl-C."
    GCS_DESTINATION="$LAPTOP" "$GCS_SCRIPT" run &
    GCS_PID=$!
    wait "$GCS_PID" || true
    GCS_PID=
else
    if [ ! -f "$TEE_SCRIPT" ]; then
        echo "gcs: $TEE_SCRIPT 가 없다" >&2
        exit 1
    fi
    python3 "$TEE_SCRIPT" "$TEE_PORT" "127.0.0.1:$PORT" "$LAPTOP" >"$TEE_LOG" 2>&1 &
    TEE_PID=$!
    sleep 1
    GCS_DESTINATION="127.0.0.1:$TEE_PORT" "$GCS_SCRIPT" run >"$GCS_LOG" 2>&1 &
    GCS_PID=$!
    sleep 1
    "$BIN" "$PORT"
fi
