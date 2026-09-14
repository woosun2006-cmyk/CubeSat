#!/bin/sh
set -eu

# 파이의 텔레메트리를 LoRa(E220) 로도 내보낸다.
#
#   ./gcs_LoRa.sh          LoRa + 노트북 UDP + 파이 GUI (기본)
#   ./gcs_LoRa.sh 10       위와 같되 LTE 를 10 Hz 로 (LoRa 는 자기 주기 유지)
#   ./gcs_LoRa.sh nogui    GUI 없이. ssh 로 띄워두고 나갈 때.
#   ./gcs_LoRa.sh solo     노트북으로 보내지 않고 LoRa + GUI 만
#   ./gcs_LoRa.sh build    gcs 와 gui 바이너리 빌드
#   ./gcs_LoRa.sh status   설정 확인
#
# 보내는 방식은 gcs_LTE.sh 와 같다. 같은 gcs 프로그램이 만든 같은 JSON 한 줄이
# 같은 seq 를 달고 UDP 와 LoRa 양쪽으로 나간다. 지상국은 어느 링크로 받든 파서
# 하나면 되고, 양쪽 seq 를 맞춰 보면 링크별 손실률이 그대로 나온다.
#
#   gcs -+-> 127.0.0.1:tee_port -> tee.py -+-> 127.0.0.1:telemetry_port (GUI)
#        |                                 +-> gcs_host:telemetry_port (노트북)
#        +-> /dev/ttyAMA3 @ 9600 (E220) ))) LoRa ))) 지상국 E220
#
# LoRa 를 켜는 일은 gcs 에 GCS_LORA_DEVICE 를 넘기는 것이 전부다. 넘기지 않으면
# gcs 는 예전처럼 LTE 로만 보낸다. gcs.service 가 그렇게 돈다.
#
# 한 UART 에 두 프로그램이 쓰면 줄이 섞인다. ttyAMA3 은 mav.py(mavlink.service)
# 가 자기 CSV 를 1 Hz 로 쓰던 자리이므로, 이 스크립트가 도는 동안에는 그 서비스를
# 멈추고 끝날 때 원래 상태로 되돌린다. gcs.service 도 픽스호크 시리얼 때문에
# 같은 이유로 멈춘다.
#
# 서비스 제어에는 sudo 가 필요하다. NOPASSWD 가 없으므로 비밀번호를 한 번
# 물어본다. ssh 로 명령만 던지면 물어볼 화면이 없으니 ssh -t 로 붙거나 미리
# sudo -v 를 해 둘 것.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$SCRIPT_DIR/gcs.c" ]; then
    PROJECT_DIR=$SCRIPT_DIR
else
    PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../gcs" && pwd)
fi
. "$PROJECT_DIR/conf.sh"

# 사람마다 다른 값(노트북 주소, 포트)은 conf.sh 가 여는 connecting_port.yaml 에,
# 기체 장치(LoRa UART)는 setting/port.yaml 에 있다. 뒤쪽은 CONF_FILE 을 잠깐
# 바꿔 같은 conf() 로 읽는다. 규칙이 같은 파일이라 파서를 하나 더 둘 이유가 없다.
PORT_CONF=${PORT_CONF:-$PROJECT_DIR/../setting/port.yaml}
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

PORT=${GUI_PORT:-$(conf telemetry_port 14550)}
TEE_PORT=${TEE_PORT:-$(conf tee_port 14553)}
LAPTOP="$(conf gcs_host 10.0.0.16):$(conf telemetry_port 14550)"
LORA_DEVICE=${GCS_LORA_DEVICE:-$(pconf lora_device /dev/ttyAMA3)}
LORA_BAUD=${GCS_LORA_BAUD:-$(pconf lora_baud 9600)}
GCS_LOG=${GCS_LOG:-/tmp/cubesat-gcs-lora.log}
TEE_LOG=${TEE_LOG:-/tmp/cubesat-tee.log}

MODE=${1:-run}
# port.yaml 은 LoRa 를 주기(초)로 적어 둔다. 여기서는 주사율로 쓰므로 뒤집는다.
SEND_HZ=${GCS_SEND_HZ:-$(awk -v v="$(pconf lora_send_interval 1)" \
    'BEGIN { printf "%g", (v > 0 ? 1 / v : 1) }')}

# 숫자 인자는 주사율. gcs.sh 에는 환경변수로 넘어간다.
if is_hz "${1:-}"; then
    SEND_HZ=$1
    MODE=run
elif is_hz "${2:-}"; then
    SEND_HZ=$2
fi
export GCS_SEND_HZ=$SEND_HZ

# gcs 에게 LoRa 를 열라고 알려 주는 유일한 스위치.
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
GCS_PID=
TEE_PID=
CLEANED=0

cleanup() {
    # Ctrl-C 는 INT 와 EXIT 를 잇달아 때린다. 서비스를 두 번 올리지 않는다.
    if [ "$CLEANED" -eq 1 ]; then
        return 0
    fi
    CLEANED=1
    [ -n "$TEE_PID" ] && kill "$TEE_PID" 2>/dev/null || true
    [ -n "$GCS_PID" ] && kill "$GCS_PID" 2>/dev/null || true
    [ -n "$TEE_PID" ] && wait "$TEE_PID" 2>/dev/null || true
    [ -n "$GCS_PID" ] && wait "$GCS_PID" 2>/dev/null || true
    if [ "$SERVICE_WAS_ACTIVE" -eq 1 ]; then
        echo "gcs_LoRa: gcs.service 를 다시 시작한다."
        svc start gcs || \
            echo "gcs_LoRa: 자동 복구 실패. sudo systemctl start gcs 를 직접 실행할 것." >&2
    fi
    if [ "$MAVLINK_WAS_ACTIVE" -eq 1 ]; then
        echo "gcs_LoRa: mavlink.service 를 다시 시작한다."
        svc start mavlink || \
            echo "gcs_LoRa: 자동 복구 실패. sudo systemctl start mavlink 를 직접 실행할 것." >&2
    fi
}

case "$MODE" in
    build)
        "$GCS_SCRIPT" build
        build_gui
        echo "gcs_LoRa: built $GCS_BIN, $BIN"
        exit 0
        ;;
    status)
        echo "설정 파일: $CONF_FILE"
        echo "장치 파일: $PORT_CONF"
        echo "활성 지상국: $(conf active -)"
        echo "LoRa 장치: $LORA_DEVICE @ $LORA_BAUD"
        if [ -c "$LORA_DEVICE" ]; then
            echo "LoRa 장치 상태: 있음"
        else
            echo "LoRa 장치 상태: 없음 (config.txt 의 dtoverlay=uart3 확인)"
        fi
        echo "GUI 수신 포트: $PORT"
        echo "중계 수신 포트: $TEE_PORT"
        echo "노트북 목적지: $LAPTOP"
        echo "송신 속도: LTE $SEND_HZ Hz / LoRa $LORA_HZ Hz"
        echo "mavlink.service: $(systemctl is-active mavlink 2>/dev/null || true)  (돌고 있으면 ttyAMA3 을 쥐고 있다)"
        echo "gcs.service: $(systemctl is-active gcs 2>/dev/null || true)"
        if [ -x "$GCS_BIN" ]; then
            echo "gcs 바이너리: $GCS_BIN"
        else
            echo "gcs 바이너리: 빌드 안 됨"
        fi
        if [ -x "$BIN" ]; then
            echo "gui 바이너리: $BIN"
        else
            echo "gui 바이너리: 빌드 안 됨"
        fi
        exit 0
        ;;
    run|solo|nogui) ;;
    *)
        echo "usage: $0 {run|solo|nogui|build|status} [hz]" >&2
        echo "       $0 <hz>" >&2
        exit 2
        ;;
esac

# GUI 는 ncurses 라 터미널이 있어야 뜬다. ssh 로 명령만 던지면 여기서 걸러야
# 서비스를 멈췄다 되돌리는 헛수고를 안 한다.
if [ "$MODE" != "nogui" ] && [ ! -t 1 ]; then
    echo "gcs_LoRa: 터미널이 없어 GUI 를 못 띄운다. nogui 로 실행하거나 ssh -t 로 붙을 것." >&2
    exit 2
fi


# 병목은 UART 9600 이 아니라 E220 의 무선 속도다. 패킷 한 줄은 실측 262 B 고,
# 기본 에어레이트 2.4 kbps 로는 한 줄을 내보내는 데만 0.87 초가 든다. 1 Hz 도
# 여유가 거의 없다는 뜻이다. 더 자주 보내야 하면 주기가 아니라 E220 의
# 에어레이트를 먼저 올릴 것. LTE 주사율은 여기 걸리지 않는다.
if ! awk -v v="$LORA_HZ" 'BEGIN { exit !(v <= 1) }'; then
    echo "gcs_LoRa: 경고 - LoRa $LORA_HZ Hz 는 과하다. 한 줄 262 B, 2.4 kbps 면 1 Hz 가 한계." >&2
fi

if [ ! -c "$LORA_DEVICE" ]; then
    echo "gcs_LoRa: LoRa 장치 $LORA_DEVICE 가 없다." >&2
    echo "          /boot/firmware/config.txt 에 dtoverlay=uart3 이 있는지 볼 것." >&2
    exit 1
fi

[ -x "$GCS_BIN" ] || "$GCS_SCRIPT" build
if [ "$MODE" != "nogui" ]; then
    [ -x "$BIN" ] || build_gui
fi

# ttyAMA3 을 mav.py 가 쓰고 있다. 한 UART 에 둘이 쓰면 두 줄이 섞여 나가므로
# 잠시 넘겨받는다. ELRS 송신도 mav.py 가 하므로 그동안 같이 멈춘다.
if systemctl is-active --quiet mavlink 2>/dev/null && ! service_is_elrs_only; then
    echo "gcs_LoRa: mavlink.service 를 잠시 멈춘다 (종료 시 자동 복구). ttyAMA3 을 넘겨받는다."
    if ! svc stop mavlink; then
        echo "gcs_LoRa: 서비스를 멈추지 못했다. sudo systemctl stop mavlink 후 다시 실행할 것." >&2
        exit 1
    fi
    MAVLINK_WAS_ACTIVE=1
    # UART 가 완전히 놓일 때까지 잠깐 기다린다.
    sleep 1
fi
trap cleanup EXIT INT TERM

# 서비스가 아니라 손으로 띄워둔 mav.py 는 스크립트가 건드리지 않는다. 다만
# 서비스를 막 멈춘 직후에는 그 파이썬이 아직 정리 중일 수 있어서, 사라지기를
# 몇 초 기다려 보고 그래도 남아 있을 때만 사람 손을 탄 것으로 본다.
waited=0
while ! service_is_elrs_only && pgrep -f "python.*mav[.]py" >/dev/null 2>&1; do
    if [ "$waited" -ge 5 ]; then
        echo "gcs_LoRa: 손으로 띄운 mav.py 가 돌고 있다. ttyAMA3 을 같이 쓸 수 없으니 먼저 종료할 것." >&2
        exit 1
    fi
    sleep 1
    waited=$((waited + 1))
done

# 픽스호크 시리얼도 두 프로세스가 열 수 없다. gui.sh 와 같은 이유다.
if systemctl is-active --quiet gcs 2>/dev/null; then
    echo "gcs_LoRa: gcs.service 를 잠시 멈춘다 (종료 시 자동 복구)."
    if ! svc stop gcs; then
        echo "gcs_LoRa: 서비스를 멈추지 못했다. sudo systemctl stop gcs 후 다시 실행할 것." >&2
        exit 1
    fi
    SERVICE_WAS_ACTIVE=1
    sleep 1
fi

if pgrep -x gcs >/dev/null 2>&1; then
    echo "gcs_LoRa: 이미 실행 중인 gcs 가 있다. 먼저 종료할 것." >&2
    exit 1
fi

echo "gcs_LoRa: $LORA_DEVICE @ $LORA_BAUD 로 내보낸다. LTE $SEND_HZ Hz / LoRa $LORA_HZ Hz"

case "$MODE" in
    nogui)
        # 화면 없이 LoRa 와 노트북 UDP 로만. gcs 로그가 그대로 흘러나온다.
        echo "gcs_LoRa: 노트북 $LAPTOP 으로도 보낸다. 멈추려면 Ctrl-C."
        GCS_DESTINATION="$LAPTOP" "$GCS_SCRIPT" run &
        GCS_PID=$!
        wait "$GCS_PID" || true
        GCS_PID=
        ;;
    solo)
        echo "gcs_LoRa: solo 모드 - 노트북으로 보내지 않는다."
        GCS_DESTINATION="127.0.0.1:$PORT" "$GCS_SCRIPT" run >"$GCS_LOG" 2>&1 &
        GCS_PID=$!
        sleep 1
        "$BIN" "$PORT"
        ;;
    run)
        if [ ! -f "$TEE_SCRIPT" ]; then
            echo "gcs_LoRa: $TEE_SCRIPT 가 없다" >&2
            exit 1
        fi
        echo "gcs_LoRa: 텔레메트리를 GUI 와 $LAPTOP 양쪽으로도 보낸다."
        python3 "$TEE_SCRIPT" "$TEE_PORT" "127.0.0.1:$PORT" "$LAPTOP" >"$TEE_LOG" 2>&1 &
        TEE_PID=$!
        sleep 1
        GCS_DESTINATION="127.0.0.1:$TEE_PORT" "$GCS_SCRIPT" run >"$GCS_LOG" 2>&1 &
        GCS_PID=$!
        sleep 1
        "$BIN" "$PORT"
        ;;
esac
