#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$SCRIPT_DIR/GUI.c" ]; then
    PROJECT_DIR=$SCRIPT_DIR
else
    PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../gcs" && pwd)
fi
BIN="$PROJECT_DIR/gui"
PORT=${GUI_PORT:-14550}
GCS_SCRIPT="$PROJECT_DIR/gcs.sh"
GCS_DESTINATION=${GCS_DESTINATION:-127.0.0.1:$PORT}
GCS_LOG=${GCS_LOG:-/tmp/cubesat-gcs.log}

build() {
    gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
        "$PROJECT_DIR/GUI.c" -lncurses -o "$BIN"
}

case "${1:-run}" in
    build)
        build
        echo "gui: built $BIN"
        ;;
    run)
        [ -x "$BIN" ] || build
        if [ ! -x "$GCS_SCRIPT" ]; then
            echo "gui: missing $GCS_SCRIPT" >&2
            exit 1
        fi
        GCS_STARTED=0
        if ! pgrep -x gcs >/dev/null 2>&1; then
            echo "gui: starting gcs.sh -> $GCS_DESTINATION"
            GCS_DESTINATION="$GCS_DESTINATION" "$GCS_SCRIPT" run >"$GCS_LOG" 2>&1 &
            GCS_PID=$!
            GCS_STARTED=1
            sleep 1
        else
            echo "gui: gcs is already running"
            GCS_PID=
        fi
        cleanup() {
            if [ "$GCS_STARTED" -eq 1 ] && [ -n "${GCS_PID:-}" ]; then
                kill "$GCS_PID" 2>/dev/null || true
                wait "$GCS_PID" 2>/dev/null || true
            fi
        }
        trap cleanup EXIT INT TERM
        "$BIN" "$PORT"
        ;;
    status)
        if [ -x "$BIN" ]; then
            echo "gui binary: $BIN"
            echo "UDP listen port: $PORT"
        else
            echo "gui binary: not built"
            exit 1
        fi
        ;;
    *)
        echo "usage: $0 {build|run|status}" >&2
        exit 2
        ;;
esac
