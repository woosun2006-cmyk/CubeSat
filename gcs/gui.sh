#!/bin/sh
set -eu

# 파이에서 ncurses GUI 로 텔레메트리를 본다.
#
#   ./gui.sh          노트북 수신을 유지한 채로 본다 (기본)
#   ./gui.sh 10       위와 같되 10 Hz 로 송신
#   ./gui.sh solo     노트북으로 보내지 않고 파이에서만 본다
#   ./gui.sh solo 10  solo 를 10 Hz 로
#   ./gui.sh build    gui 바이너리만 빌드
#   ./gui.sh status   설정 확인
#
# gcs.c 는 목적지를 하나만 가지므로, 기본 모드에서는 gcs 를 루프백으로 보내게
# 하고 tee.py 가 그것을 GUI 와 노트북 양쪽으로 복제한다.
#
#   gcs -> 127.0.0.1:tee_port -> tee.py -+-> 127.0.0.1:telemetry_port (GUI)
#                                        +-> gcs_host:telemetry_port (노트북)
#
# gcs.service 가 돌고 있으면 잠시 멈췄다가 GUI 를 닫을 때 원래 상태로
# 되돌린다. 서비스와 이 스크립트가 같은 시리얼 장치를 동시에 열면 바이트를
# 나눠 가져 양쪽 다 파싱이 깨지기 때문이다.
#
# 서비스 제어에는 sudo 가 필요하다. NOPASSWD 가 설정되어 있지 않으므로
# 비밀번호를 한 번 물어본다. 같은 터미널에서 다시 실행하면 sudo 타임스탬프가
# 남아 있어 대개 묻지 않는다.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$SCRIPT_DIR/GUI.c" ]; then
    PROJECT_DIR=$SCRIPT_DIR
else
    PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../gcs" && pwd)
fi
. "$PROJECT_DIR/conf.sh"

BIN="$PROJECT_DIR/gui"
GCS_SCRIPT="$PROJECT_DIR/gcs.sh"
TEE_SCRIPT="$PROJECT_DIR/tee.py"

PORT=${GUI_PORT:-$(conf telemetry_port 14550)}
TEE_PORT=${TEE_PORT:-$(conf tee_port 14553)}
LAPTOP="$(conf gcs_host 10.0.0.16):$(conf telemetry_port 14550)"
GCS_LOG=${GCS_LOG:-/tmp/cubesat-gcs.log}
TEE_LOG=${TEE_LOG:-/tmp/cubesat-tee.log}

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

build() {
    gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
        "$PROJECT_DIR/GUI.c" -lncurses -o "$BIN"
}

# 비밀번호 없이 되면 그대로 쓰고, 안 되면 한 번 물어본다.
svc() {
    if sudo -n systemctl "$1" gcs 2>/dev/null; then
        return 0
    fi
    sudo systemctl "$1" gcs
}

SERVICE_WAS_ACTIVE=0
GCS_PID=
TEE_PID=

cleanup() {
    [ -n "$TEE_PID" ] && kill "$TEE_PID" 2>/dev/null || true
    [ -n "$GCS_PID" ] && kill "$GCS_PID" 2>/dev/null || true
    [ -n "$TEE_PID" ] && wait "$TEE_PID" 2>/dev/null || true
    [ -n "$GCS_PID" ] && wait "$GCS_PID" 2>/dev/null || true
    if [ "$SERVICE_WAS_ACTIVE" -eq 1 ]; then
        echo "gui: gcs.service 를 다시 시작한다."
        svc start || \
            echo "gui: 자동 복구 실패. 직접 'sudo systemctl start gcs' 를 실행할 것." >&2
    fi
}

case "$MODE" in
    build)
        build
        echo "gui: built $BIN"
        exit 0
        ;;
    status)
        echo "설정 파일: $CONF_FILE"
        echo "활성 지상국: $(conf active -)"
        echo "GUI 수신 포트: $PORT"
        echo "중계 수신 포트: $TEE_PORT"
        echo "노트북 목적지: $LAPTOP"
        echo "송신 속도: $SEND_HZ Hz"
        echo "로그 위치: $(cd "$PROJECT_DIR/../log/gcs" 2>/dev/null && pwd || echo "$PROJECT_DIR/../log/gcs")"
        echo "gcs.service: $(systemctl is-active gcs 2>/dev/null || true)"
        if [ -x "$BIN" ]; then
            echo "gui 바이너리: $BIN"
        else
            echo "gui 바이너리: 빌드 안 됨"
            exit 1
        fi
        exit 0
        ;;
    run|solo) ;;
    *)
        echo "usage: $0 {run|solo|build|status} [hz]" >&2
        echo "       $0 <hz>" >&2
        exit 2
        ;;
esac

[ -x "$BIN" ] || build
if [ ! -x "$GCS_SCRIPT" ]; then
    echo "gui: $GCS_SCRIPT 가 없다" >&2
    exit 1
fi

# 서비스가 돌고 있으면 멈춘다. 같은 시리얼 장치를 두 프로세스가 열 수 없다.
if systemctl is-active --quiet gcs 2>/dev/null; then
    echo "gui: gcs.service 를 잠시 멈춘다 (GUI 종료 시 자동 복구)."
    if ! svc stop; then
        echo "gui: 서비스를 멈추지 못했다. 'sudo systemctl stop gcs' 후 다시 실행할 것." >&2
        exit 1
    fi
    SERVICE_WAS_ACTIVE=1
    # 시리얼이 완전히 놓일 때까지 잠깐 기다린다.
    sleep 1
fi
trap cleanup EXIT INT TERM

# 혹시 손으로 띄워둔 gcs 가 있으면 먼저 정리해야 한다.
if pgrep -x gcs >/dev/null 2>&1; then
    echo "gui: 이미 실행 중인 gcs 가 있다. 먼저 종료할 것." >&2
    exit 1
fi

if [ "$MODE" = "solo" ]; then
    echo "gui: solo 모드 - 노트북으로 보내지 않는다."
    GCS_DESTINATION="127.0.0.1:$PORT" "$GCS_SCRIPT" run >"$GCS_LOG" 2>&1 &
    GCS_PID=$!
else
    if [ ! -f "$TEE_SCRIPT" ]; then
        echo "gui: $TEE_SCRIPT 가 없다" >&2
        exit 1
    fi
    echo "gui: 텔레메트리를 GUI 와 $LAPTOP 양쪽으로 보낸다. ($SEND_HZ Hz)"
    python3 "$TEE_SCRIPT" "$TEE_PORT" "127.0.0.1:$PORT" "$LAPTOP" >"$TEE_LOG" 2>&1 &
    TEE_PID=$!
    sleep 1
    GCS_DESTINATION="127.0.0.1:$TEE_PORT" "$GCS_SCRIPT" run >"$GCS_LOG" 2>&1 &
    GCS_PID=$!
fi

sleep 1
"$BIN" "$PORT"
