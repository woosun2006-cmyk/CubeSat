#!/bin/sh
set -eu

# 파이에서 Pixhawk 텔레메트리를 읽어 노트북으로 보낸다.
#
#   ./gcs.sh          yaml 의 send_hz 로 송신 (기본 1 Hz)
#   ./gcs.sh 10       10 Hz 로 송신
#   ./gcs.sh run 10   위와 같음
#   ./gcs.sh build    빌드만
#   ./gcs.sh status   설정 확인

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$SCRIPT_DIR/gcs.c" ]; then
    PROJECT_DIR=$SCRIPT_DIR
else
    PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../gcs" && pwd)
fi
. "$PROJECT_DIR/conf.sh"

BIN="$PROJECT_DIR/gcs"
DEVICE=${PIXHAWK_DEVICE:-$(conf pixhawk_device /dev/ttyACM0)}
BAUD=${PIXHAWK_BAUD:-$(conf pixhawk_baud 460800)}
DESTINATION=${GCS_DESTINATION:-$(conf gcs_host 10.0.0.16):$(conf telemetry_port 14550)}
SEND_HZ=${GCS_SEND_HZ:-$(conf send_hz 1)}
# 텔레메트리 원본을 남길 곳. 어디서 실행하든 같은 자리를 가리키도록
# PROJECT_DIR 기준으로 풀어서 절대 경로로 넘긴다.
LOG_DIR=${GCS_LOG_DIR:-$(conf log_dir "$PROJECT_DIR/../log/gcs")}
# .. 가 섞인 경로는 로그와 화면에 그대로 찍혀 읽기 나쁘다. 풀어서 넘긴다.
LOG_DIR=$(mkdir -p "$LOG_DIR" 2>/dev/null && cd "$LOG_DIR" && pwd) || \n    LOG_DIR=${GCS_LOG_DIR:-$PROJECT_DIR/../log/gcs}
export GCS_LOG_DIR="$LOG_DIR"

# 첫 인자가 숫자면 그것이 주사율이고 동작은 run 이다. "run 10" 도 받는다.
ACTION=${1:-run}
if is_hz "${1:-}"; then
    SEND_HZ=$1
    ACTION=run
elif [ "${1:-}" = "run" ] && is_hz "${2:-}"; then
    SEND_HZ=$2
fi

build() {
    gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
        "$PROJECT_DIR/gcs.c" "$PROJECT_DIR/data.c" "$PROJECT_DIR/MAVLink.c" -o "$BIN"
}

case "$ACTION" in
    build)
        build
        echo "gcs: built $BIN"
        ;;
    run)
        [ -x "$BIN" ] || build
        exec "$BIN" "$DEVICE" "$DESTINATION" "$BAUD" "$SEND_HZ"
        ;;
    status)
        echo "설정 파일: $CONF_FILE"
        echo "활성 지상국: $(conf active -)  $(conf description -)"
        echo "Pixhawk: $DEVICE @ $BAUD"
        echo "UDP 목적지: $DESTINATION"
        echo "송신 속도: $SEND_HZ Hz"
        echo "로그 위치: $LOG_DIR"
        if [ -x "$BIN" ]; then
            echo "gcs 바이너리: $BIN"
        else
            echo "gcs 바이너리: 빌드 안 됨"
            exit 1
        fi
        ;;
    *)
        echo "usage: $0 {build|run|status}" >&2
        exit 2
        ;;
esac
