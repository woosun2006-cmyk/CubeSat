#!/bin/sh
set -u

# 짐벌 구동과 카메라 녹화를 함께 돌린다.
#
#   ./record.sh 10min     10분 녹화
#   ./record.sh 10sec     10초 녹화
#   ./record.sh 1h        1시간 녹화
#   ./record.sh 90        단위를 안 붙이면 초로 본다
#   ./record.sh status    설정 확인
#   ./record.sh build     짐벌만 빌드
#
# 녹화 시간이 끝나면 짐벌도 함께 멈춘다. gimbal_full 은 인자로 받은 시간이
# 없고 while(1) 로 돌며 시그널 처리도 없으므로, 종료는 이 스크립트가 맡는다.
#
# 짐벌은 Pixhawk 의 두 번째 MAVLink 포트(ttyACM1)를 쓴다. gcs 가 ttyACM0 을
# 잡고 있어도 서로 건드리지 않으므로, 이 스크립트는 gcs.service 를 멈추지
# 않는다. 같은 장치를 두 프로세스가 열면 바이트를 나눠 가져 양쪽 파싱이
# 깨지기 때문에 포트를 나누는 쪽이 맞다.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

GIMBAL_DIR="$PROJECT_DIR/camera/gimbal"
GIMBAL_BIN="$GIMBAL_DIR/bin/gimbal_full"
GIMBAL_SRC="$GIMBAL_DIR/gimbal_full.c"
RECORD_SCRIPT="$PROJECT_DIR/camera/record_video.sh"
GIMBAL_SERIAL=${GIMBAL_SERIAL:-/dev/ttyACM1}
GIMBAL_LOG=${GIMBAL_LOG:-/tmp/cubesat-gimbal.log}

# 단위가 붙은 시간을 초로 바꾼다. min 을 m 보다, sec 을 s 보다 먼저 봐야 한다.
duration_seconds() {
    raw=$1
    case "$raw" in
        *min) value=${raw%min}; scale=60 ;;
        *sec) value=${raw%sec}; scale=1 ;;
        *h)   value=${raw%h};   scale=3600 ;;
        *m)   value=${raw%m};   scale=60 ;;
        *s)   value=${raw%s};   scale=1 ;;
        *)    value=$raw;       scale=1 ;;
    esac
    case "$value" in
        ''|*[!0-9.]*|*.*.*) return 1 ;;
    esac
    awk -v v="$value" -v s="$scale" \
        'BEGIN { if (v <= 0) exit 1; printf "%.0f\n", v * s }'
}

build_gimbal() {
    echo "record: gimbal_full 을 빌드한다."
    mkdir -p "$GIMBAL_DIR/bin"
    # wiringPi 와 MAVLink 헤더는 ~/.local 에 있다 (camera/gimbal/README.md).
    gcc -Wall -I "$HOME/.local/include" -L "$HOME/.local/lib" \
        "$GIMBAL_SRC" -o "$GIMBAL_BIN" -lwiringPi -lpthread
}

MODE=${1:-}

case "$MODE" in
    ""|-h|--help|help)
        echo "usage: $0 <시간>   예: 10min, 10sec, 1h, 90" >&2
        echo "       $0 status | build" >&2
        exit 2
        ;;
    status)
        echo "짐벌 소스    : $GIMBAL_SRC"
        echo "짐벌 바이너리: $GIMBAL_BIN"
        echo "짐벌 MAVLink : $GIMBAL_SERIAL"
        echo "녹화 스크립트: $RECORD_SCRIPT"
        echo "저장 위치    : $PROJECT_DIR/log/camera"
        echo "gcs.service  : $(systemctl is-active gcs 2>/dev/null || true)"
        if [ -x "$GIMBAL_BIN" ]; then
            echo "짐벌 상태    : 빌드됨"
        else
            echo "짐벌 상태    : 빌드 안 됨"
        fi
        exit 0
        ;;
    build)
        build_gimbal
        echo "record: built $GIMBAL_BIN"
        exit 0
        ;;
esac

if ! SECONDS_TOTAL=$(duration_seconds "$MODE"); then
    echo "record: 시간을 알 수 없다: '$MODE'" >&2
    echo "        10min / 10sec / 1h / 90 처럼 써야 한다." >&2
    exit 2
fi

if [ ! -f "$GIMBAL_SRC" ]; then
    echo "record: $GIMBAL_SRC 가 없다" >&2
    exit 1
fi
if [ ! -x "$RECORD_SCRIPT" ]; then
    echo "record: $RECORD_SCRIPT 가 없다" >&2
    exit 1
fi
if [ ! -x "$GIMBAL_BIN" ] || [ "$GIMBAL_SRC" -nt "$GIMBAL_BIN" ]; then
    build_gimbal
fi
if [ ! -e "$GIMBAL_SERIAL" ]; then
    echo "record: $GIMBAL_SERIAL 이 없다. Pixhawk USB 를 확인할 것." >&2
    exit 1
fi

GIMBAL_PID=

cleanup() {
    if [ -n "$GIMBAL_PID" ] && kill -0 "$GIMBAL_PID" 2>/dev/null; then
        echo "record: 짐벌 정지"
        kill "$GIMBAL_PID" 2>/dev/null || true
        wait "$GIMBAL_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

echo "record: 짐벌 시작 — $GIMBAL_SERIAL (로그 $GIMBAL_LOG)"
"$GIMBAL_BIN" "$GIMBAL_SERIAL" >"$GIMBAL_LOG" 2>&1 &
GIMBAL_PID=$!

# 하트비트를 기다리는 구간이 있어 곧바로 죽을 수 있다. 확인하고 넘어간다.
sleep 2
if ! kill -0 "$GIMBAL_PID" 2>/dev/null; then
    GIMBAL_PID=
    echo "record: 짐벌이 바로 종료됐다. $GIMBAL_LOG 확인:" >&2
    tail -n 5 "$GIMBAL_LOG" >&2 2>/dev/null || true
    exit 1
fi

echo "record: ${SECONDS_TOTAL}초 녹화 시작"
"$RECORD_SCRIPT" "$SECONDS_TOTAL"
STATUS=$?

echo "record: 녹화 종료"
exit "$STATUS"
