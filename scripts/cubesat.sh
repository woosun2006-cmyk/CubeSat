#!/bin/sh
set -u

# 전체 미션: 텔레메트리 GUI 와 짐벌+녹화를 함께 돌린다.
#
#   ./cubesat.sh --50Hz --10min    50 Hz 로 보내며 화면을 띄우고 10분 녹화
#   ./cubesat.sh --10Hz --30sec
#   ./cubesat.sh --10min           주사율은 yaml 의 send_hz
#   ./cubesat.sh status
#
# 순서는 상관없고 대소문자도 가리지 않는다.
#
# 녹화는 뒤에서 돌고, 화면(gcs.sh)이 터미널을 차지한다. 화면을 닫으면 녹화도
# 함께 정리한다. 녹화가 먼저 끝나면 화면은 그대로 남으므로 원할 때 닫으면 된다.
#
# 두 프로그램이 Pixhawk 의 서로 다른 MAVLink 포트를 쓰기 때문에 같이 돌 수
# 있다. gcs 는 ttyACM0, 짐벌은 ttyACM1 이다. 한 장치를 둘이 열면 바이트를
# 나눠 가져 양쪽 파싱이 모두 깨지므로 이 분리가 전제 조건이다.
#
# gcs.service 정지와 복구는 gcs.sh 가 이미 하므로 여기서 또 건드리지 않는다.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

GCS_SCRIPT="$SCRIPT_DIR/gcs.sh"
RECORD_SCRIPT="$SCRIPT_DIR/record.sh"
RECORD_LOG=${RECORD_LOG:-/tmp/cubesat-record.log}

HZ=
DURATION=

for arg in "$@"; do
    case "$arg" in
        status)
            "$GCS_SCRIPT" status 2>/dev/null | sed 's/^/  /'
            echo
            "$RECORD_SCRIPT" status 2>/dev/null | sed 's/^/  /'
            exit 0
            ;;
        -h|--help|help)
            echo "usage: $0 --<주사율>Hz --<시간>" >&2
            echo "  예: $0 --50Hz --10min   (순서 무관)" >&2
            exit 2
            ;;
        --*[Hh][Zz])
            HZ=${arg#--}
            HZ=${HZ%??}
            ;;
        --*)
            DURATION=${arg#--}
            ;;
        *)
            echo "cubesat: 알 수 없는 인자 '$arg'" >&2
            exit 2
            ;;
    esac
done

if [ -z "$DURATION" ]; then
    echo "cubesat: 녹화 시간이 없다. 예: --10min" >&2
    exit 2
fi
if [ ! -x "$GCS_SCRIPT" ] || [ ! -x "$RECORD_SCRIPT" ]; then
    echo "cubesat: gcs.sh 또는 record.sh 를 찾을 수 없다." >&2
    exit 1
fi

RECORD_PID=

cleanup() {
    [ -n "$RECORD_PID" ] || return 0
    kill -0 "$RECORD_PID" 2>/dev/null || return 0
    echo "cubesat: 녹화 정리"
    # record.sh 는 카메라와 짐벌을 자식으로 둔다. 프로세스 그룹째 보내야
    # rpicam 이 혼자 남지 않는다 (setsid 로 그룹을 따로 떼어 두었다).
    kill -TERM "-$RECORD_PID" 2>/dev/null || kill -TERM "$RECORD_PID" 2>/dev/null || true
    # 카메라가 mp4 를 마무리할 시간을 준다. 예전에는 1 초 뒤 KILL 해서
    # 영상 끝부분(moov)이 안 써진 재생 불가 파일이 남았다. 보통 1 초 안에
    # 끝나지만 인코더가 밀려 있으면 더 걸린다.
    _waited=0
    while kill -0 "$RECORD_PID" 2>/dev/null && [ "$_waited" -lt 15 ]; do
        sleep 1
        _waited=$((_waited + 1))
    done
    kill -KILL "-$RECORD_PID" 2>/dev/null || true
    wait "$RECORD_PID" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT TERM

echo "cubesat: 녹화 시작 — $DURATION (로그 $RECORD_LOG)"
if command -v setsid >/dev/null 2>&1; then
    setsid "$RECORD_SCRIPT" "$DURATION" >"$RECORD_LOG" 2>&1 &
else
    "$RECORD_SCRIPT" "$DURATION" >"$RECORD_LOG" 2>&1 &
fi
RECORD_PID=$!

# 시간 형식이 틀렸거나 짐벌이 안 뜨면 곧바로 죽는다. 화면을 띄우기 전에 본다.
sleep 3
if ! kill -0 "$RECORD_PID" 2>/dev/null; then
    wait "$RECORD_PID"
    STATUS=$?
    RECORD_PID=
    if [ "$STATUS" -ne 0 ]; then
        echo "cubesat: 녹화가 시작되지 못했다. $RECORD_LOG 확인:" >&2
        tail -n 5 "$RECORD_LOG" >&2 2>/dev/null || true
        exit "$STATUS"
    fi
    echo "cubesat: 녹화가 이미 끝났다 (짧은 시간)."
fi

if [ -n "$HZ" ]; then
    echo "cubesat: 텔레메트리 ${HZ} Hz — 화면을 닫으면 녹화도 정리된다."
    "$GCS_SCRIPT" "$HZ"
else
    echo "cubesat: 텔레메트리 시작 (주사율은 설정값)"
    "$GCS_SCRIPT" run
fi
STATUS=$?

echo "cubesat: 완료"
exit "$STATUS"
