#!/usr/bin/env python3
"""gcs 가 보낸 UDP 패킷을 여러 곳으로 복제한다.

gcs.c 는 목적지를 하나만 가진다. 파이에서 ncurses gui 를 보는 동안에도
노트북이 계속 텔레메트리를 받으려면 중간에서 복제해 주어야 한다.

    tee.py <수신포트> <대상1> [대상2 ...]
    tee.py 14553 127.0.0.1:14550 10.0.0.16:14550

루프백에서만 받으므로 외부에 새 포트를 여는 것이 아니다.
"""
import signal
import socket
import sys

def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2

    listen_port = int(argv[1])
    targets = []
    for item in argv[2:]:
        host, _, port = item.rpartition(":")
        if not host or not port.isdigit():
            print("잘못된 대상: %s" % item, file=sys.stderr)
            return 2
        targets.append((host, int(port)))

    running = {"on": True}
    def stop(*_):
        running["on"] = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    inbound = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    inbound.bind(("127.0.0.1", listen_port))
    inbound.settimeout(0.5)
    outbound = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    while running["on"]:
        try:
            data, _ = inbound.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        for target in targets:
            try:
                outbound.sendto(data, target)
            except OSError:
                pass

    inbound.close()
    outbound.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
