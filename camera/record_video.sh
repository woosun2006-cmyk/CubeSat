#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SOURCE="$PROJECT_DIR/camera/record/record_video.c"
BIN="$PROJECT_DIR/camera/record/record_video"

build() {
    gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
        "$SOURCE" -o "$BIN"
}

MODE=${1:-run}
case "$MODE" in
    build)
        build
        echo "record_video: built $BIN"
        # exit 없이 두면 아래 exec 로 흘러가 "build" 를 초 단위로 읽는다.
        exit 0
        ;;
    status)
        echo "소스: $SOURCE"
        echo "바이너리: $BIN"
        echo "설정: $PROJECT_DIR/setting/cam_sets.yaml"
        echo "저장 위치: $PROJECT_DIR/log/camera"
        if [ -x "$BIN" ]; then
            echo "상태: 빌드됨"
        else
            echo "상태: 빌드 안 됨"
            exit 1
        fi
        exit 0
        ;;
    run)
        shift
        ;;
    *)
        case "$MODE" in
            -*) ;;
            *[!0-9.]*)
                echo "usage: $0 [seconds] | build | status" >&2
                exit 2
                ;;
        esac
        ;;
esac

if [ ! -x "$BIN" ] || [ "$SOURCE" -nt "$BIN" ]; then
    build
fi
exec "$BIN" "$@"
