#!/bin/sh
# 노트북이 내 텔레메트리를 실제로 받고 있는지 확인한다.
#
# UDP는 보낸 쪽에 도착 여부를 알려주지 않는다. 수신자가 없을 때 돌아와야 할
# ICMP port unreachable도 허브를 넘어오지 않는다(실측: 6번 중 0번 감지).
# 그래서 노트북의 대기 에이전트가 2초마다 자기 상태를 UDP 14552로 되보내고,
# 이 스크립트는 그것을 듣는다. 보고가 없다는 사실 자체가 "노트북이 안 받고
# 있다"는 신호다.
#
#   ./link.sh          한 번 확인하고 끝  (정상 0 / 이상 1)
#   ./link.sh watch    계속 감시하며 상태가 바뀔 때만 알림
#   ./link.sh 20       20초까지 기다려 보고 끝

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$SCRIPT_DIR
. "$PROJECT_DIR/conf.sh"
PORT=${LINK_PORT:-$(conf ack_port 14552)}
exec python3 - "$PORT" "${1:-6}" <<'EOF'
import json, socket, sys, time

port = int(sys.argv[1])
arg = sys.argv[2]
watch = arg == "watch"
window = 0.0 if watch else float(arg)
SILENCE = 6.0          # 이 시간 넘게 조용하면 끊긴 것으로 본다

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
s.settimeout(1.0)

STATE = {
    "receiving": "수신 중",
    "waiting":   "대기 중 (아직 내 패킷을 못 받음)",
    "stalled":   "끊김? (세션은 열려 있으나 조용함)",
}

def now():
    return time.strftime("%H:%M:%S")

def detail(m):
    st = m.get("state")
    if st in ("receiving", "stalled"):
        return "수신 %s개 %.2fHz  마지막 %sms 전  기록 %s줄 (%s)  가동 %ss" % (
            m.get("rx"), m.get("hz", 0), m.get("age_ms"),
            m.get("logged"), m.get("log"), m.get("up_s"))
    return "%s초째" % m.get("idle_s")

def warn():
    print("")
    print("  !! 노트북이 내 텔레메트리를 받고 있지 않다 !!")
    print("     노트북에서  gcs-standby.bat  을 실행할 것.")
    print("     그래도 안 되면:  ping 10.0.0.16  으로 터널 확인.")
    print("")

print("노트북 상태 보고를 UDP %d 에서 듣는 중..." % port)
if watch:
    print("Ctrl+C 로 종료. 상태가 바뀔 때만 출력한다.")

start = time.time()
last_heard = None
last_state = None
alerted = False
deadline = None if watch else time.time() + window

try:
    while True:
        if deadline is not None and time.time() > deadline:
            break
        try:
            data, addr = s.recvfrom(2048)
            m = json.loads(data.decode("utf-8"))
            if m.get("agent") != "cubesat-gcs-standby":
                continue
        except socket.timeout:
            m = None
        except Exception:
            continue

        t = time.time()

        if m is not None:
            if alerted:
                print("[%s] 복구됨 - 노트북이 다시 응답한다." % now())
                alerted = False
            st = m.get("state")
            if not watch or st != last_state:
                print("[%s] %s  %s  %s" % (now(), addr[0],
                                           STATE.get(st, st), detail(m)))
                last_state = st
            last_heard = t
            if not watch:
                sys.exit(0 if st == "receiving" else 2)
            continue

        if not watch:
            continue

        if last_heard is not None and not alerted and t - last_heard > SILENCE:
            print("[%s] 노트북 응답 끊김 (%.0f초)" % (now(), t - last_heard))
            warn()
            alerted = True
            last_state = None
        elif last_heard is None and not alerted and deadline is None:
            # watch 시작 직후부터 아무 응답이 없는 경우
            if t - start > SILENCE:
                warn()
                alerted = True
except KeyboardInterrupt:
    print("")
finally:
    s.close()

if last_heard is None:
    warn()
    sys.exit(1)
EOF
