#!/bin/sh
set -eu

# ELRS 링크로 텔레메트리를 내보내고, 그 링크를 점검한다.
#
#   ./gcs_ELRS.sh            mav.py 를 화면에서 띄운다 (기본)
#   ./gcs_ELRS.sh monitor    crsf_monitor.py - 들어오는 CRSF 프레임 확인
#   ./gcs_ELRS.sh loopback   loopback_jih.py - UART 배선 점검
#   ./gcs_ELRS.sh probe      att_probe.py - 픽스호크 ATTITUDE 수신율 측정
#   ./gcs_ELRS.sh status     설정 확인
#
# LTE 와 LoRa 는 gcs(C) 가 보내지만 ELRS 는 mav.py 가 보낸다. 실어 나르는 것이
# JSON 한 줄이 아니라 MAVLink 메시지 자체이기 때문이다. 그래서 이 스크립트는
# gcs 를 부르지 않고 mav.py 를 앞에서 띄운다. 평소에 mavlink.service 가 하는
# 그 일을, 로그를 눈으로 보면서 하는 것이 run 모드다.
#
#   Pixhawk -if00 -+-> mav.py -+-> /dev/serial0 @460800 (MAVLink) -> ELRS TX 백팩
#                              |                                     ))) 지상국
#                              +-> /dev/ttyAMA3 @9600 (E220) ))) LoRa CSV
#
# 백팩 쪽 주소와 포트는 저장소 README 의 ExpressLRS Backpack 표에 있다.
#
# 어느 모드든 장치가 하나뿐이라 mavlink.service 와 같이 쓸 수 없다. 그래서
# 도는 동안 서비스를 멈추고 끝나면 원래 상태로 되돌린다. gcs_LoRa.sh 와 같은
# 방식이고, 그 스크립트가 ttyAMA3 을 쥐고 있는 동안에는 이쪽이 물러선다.
#
# 서비스 제어에는 sudo 가 필요하다. NOPASSWD 가 없으므로 비밀번호를 한 번
# 물어본다. ssh 로 명령만 던지면 물어볼 화면이 없으니 ssh -t 로 붙거나 미리
# sudo -v 를 해 둘 것.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$SCRIPT_DIR/mav.py" ]; then
    ELRS_DIR=$SCRIPT_DIR
else
    ELRS_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../ELRS" && pwd)
fi
ROOT=$(CDPATH= cd -- "$ELRS_DIR/.." && pwd)

# conf.sh 는 소스하기 전에 PROJECT_DIR 이 정해져 있어야 한다. 사람마다 다른
# 값(노트북 주소)은 gcs/connection/, 기체 장치는 setting/port.yaml 에 있다.
PROJECT_DIR="$ROOT/gcs"
. "$PROJECT_DIR/conf.sh"
PORT_CONF=${PORT_CONF:-$ROOT/setting/port.yaml}
pconf() {
    _saved_conf=$CONF_FILE
    CONF_FILE=$PORT_CONF
    _pvalue=$(conf "$1" "$2")
    CONF_FILE=$_saved_conf
    printf '%s\n' "$_pvalue"
}

PY=${PY:-$ROOT/mavenv/bin/python}
ELRS_DEVICE=$(pconf elrs_device /dev/serial0)
ELRS_BAUD=$(pconf elrs_baud 460800)
CRSF_DEVICE=$(pconf crsf_device /dev/serial0)
CRSF_BAUD=$(pconf crsf_baud 420000)
PIXHAWK_DEVICE=$(pconf pixhawk_device /dev/ttyACM0)
PIXHAWK_BAUD=$(pconf pixhawk_baud 115200)
LORA_DEVICE=$(pconf lora_device /dev/ttyAMA3)

MODE=${1:-run}

# 비밀번호 없이 되면 그대로 쓰고, 안 되면 한 번 물어본다.
svc() {
    if sudo -n systemctl "$1" "$2" 2>/dev/null; then
        return 0
    fi
    sudo systemctl "$1" "$2"
}

MAVLINK_WAS_ACTIVE=0
CLEANED=0

cleanup() {
    # Ctrl-C 는 INT 와 EXIT 를 잇달아 때린다. 서비스를 두 번 올리지 않는다.
    if [ "$CLEANED" -eq 1 ]; then
        return 0
    fi
    CLEANED=1
    if [ "$MAVLINK_WAS_ACTIVE" -eq 1 ]; then
        echo "gcs_ELRS: mavlink.service 를 다시 시작한다."
        svc start mavlink || \
            echo "gcs_ELRS: 자동 복구 실패. sudo systemctl start mavlink 를 직접 실행할 것." >&2
    fi
}

case "$MODE" in
    status)
        echo "설정 파일: $CONF_FILE"
        echo "장치 파일: $PORT_CONF"
        echo "ELRS 송신: $ELRS_DEVICE @ $ELRS_BAUD (mav.py)"
        echo "CRSF 수신: $CRSF_DEVICE @ $CRSF_BAUD (crsf_monitor.py)"
        echo "픽스호크: $PIXHAWK_DEVICE @ $PIXHAWK_BAUD"
        echo "LoRa: $LORA_DEVICE (mav.py 가 같이 쓴다)"
        if [ -e "$ELRS_DEVICE" ]; then
            echo "ELRS 장치 상태: 있음 ($(readlink -f "$ELRS_DEVICE"))"
        else
            echo "ELRS 장치 상태: 없음"
        fi
        echo "mavlink.service: $(systemctl is-active mavlink 2>/dev/null || true) / $(systemctl is-enabled mavlink 2>/dev/null || true)"
        echo "파이썬: $PY"
        exit 0
        ;;
    run|monitor|loopback|probe) ;;
    *)
        echo "usage: $0 {run|monitor|loopback|probe|status}" >&2
        exit 2
        ;;
esac

if [ ! -x "$PY" ]; then
    echo "gcs_ELRS: $PY 가 없다. README 의 venv 구성을 먼저 할 것." >&2
    exit 1
fi

if [ "$MODE" != "probe" ] && [ ! -e "$ELRS_DEVICE" ]; then
    echo "gcs_ELRS: ELRS 장치 $ELRS_DEVICE 가 없다." >&2
    exit 1
fi

# gcs_LoRa.sh 가 돌고 있으면 그쪽이 ttyAMA3 을 쥐고 mavlink.service 를 이미
# 멈춰 둔 상태다. 여기서 mav.py 를 띄우면 같은 UART 에 둘이 쓰게 된다.
if pgrep -f "gcs/lora[.]sh" >/dev/null 2>&1; then
    echo "gcs_ELRS: gcs_LoRa.sh 가 돌고 있다. 먼저 그쪽을 끝낼 것." >&2
    exit 1
fi

# 장치가 하나뿐이라 서비스와 같이 쓸 수 없다. 잠시 넘겨받는다.
if systemctl is-active --quiet mavlink 2>/dev/null; then
    echo "gcs_ELRS: mavlink.service 를 잠시 멈춘다 (종료 시 자동 복구)."
    if ! svc stop mavlink; then
        echo "gcs_ELRS: 서비스를 멈추지 못했다. sudo systemctl stop mavlink 후 다시 실행할 것." >&2
        exit 1
    fi
    MAVLINK_WAS_ACTIVE=1
    # 시리얼이 완전히 놓일 때까지 잠깐 기다린다.
    sleep 1
fi
trap cleanup EXIT INT TERM

# 서비스가 아니라 손으로 띄워둔 mav.py 는 스크립트가 건드리지 않는다. 다만
# 서비스를 막 멈춘 직후에는 그 파이썬이 아직 정리 중일 수 있어서, 사라지기를
# 몇 초 기다려 보고 그래도 남아 있을 때만 사람 손을 탄 것으로 본다.
waited=0
while pgrep -f "python.*mav[.]py" >/dev/null 2>&1; do
    if [ "$waited" -ge 5 ]; then
        echo "gcs_ELRS: 손으로 띄운 mav.py 가 돌고 있다. 먼저 종료할 것." >&2
        exit 1
    fi
    sleep 1
    waited=$((waited + 1))
done

case "$MODE" in
    run)
        echo "gcs_ELRS: mav.py - $ELRS_DEVICE @ $ELRS_BAUD 로 MAVLink 를 내보낸다. 멈추려면 Ctrl-C."
        "$PY" -u "$ELRS_DIR/mav.py"
        ;;
    monitor)
        # 조종기에서 오는 RC 프레임이 보이면 링크가 살아 있다는 뜻이다.
        echo "gcs_ELRS: crsf_monitor.py - $CRSF_DEVICE @ $CRSF_BAUD. 조종기 스틱을 움직여 볼 것."
        "$PY" -u "$ELRS_DIR/crsf_monitor.py"
        ;;
    loopback)
        # TX-RX 를 직결한 상태에서 보낸 문자열이 그대로 돌아오면 UART 는 정상.
        echo "gcs_ELRS: loopback_jih.py - TX 와 RX 를 직결한 상태여야 한다."
        "$PY" -u "$ELRS_DIR/loopback_jih.py"
        ;;
    probe)
        # 픽스호크에서 ATTITUDE 가 몇 Hz 로 올라오는지 본다. 약 30 초 걸린다.
        echo "gcs_ELRS: att_probe.py - $PIXHAWK_DEVICE 에서 ATTITUDE 를 30 초쯤 받아 본다."
        "$PY" -u "$ELRS_DIR/att_probe.py"
        ;;
esac
