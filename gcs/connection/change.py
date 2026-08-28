#!/usr/bin/env python3
"""connecting_port.yaml 을 사람별 CSV 값으로 바꾼다.

    python3 change.py MJ.csv     MJ 설정 적용
    python3 change.py --list     사용 가능한 CSV 목록
    python3 change.py --show     지금 값 보기

CSV는 key,value 두 열이다. 첫 줄은 헤더.

    key,value
    name,MJ
    gcs_host,10.0.0.16
    telemetry_port,14550

CSV에 없는 항목은 건드리지 않는다. 기체 쪽 설정(pixhawk_device 등)은
사람이 바뀌어도 그대로 남는다.

YAML은 한 단계 key: value 만 쓰므로 PyYAML 없이 직접 읽고 쓴다. 파이에
패키지를 더 깔지 않아도 되게 하려는 것이다.
"""

import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONF = os.path.join(HERE, "connecting_port.yaml")

# CSV로 바꿀 수 있는 항목. 오타를 걸러내려고 명시해 둔다.
ALLOWED = {
    "active", "name", "description",
    "gcs_host", "telemetry_port", "relay_port", "ack_port", "tee_port",
    "pixhawk_device", "pixhawk_baud", "send_hz", "video_fps",
}


def read_yaml(path):
    """주석과 순서를 보존하며 읽는다. 반환: (줄 목록, {key: 줄번호})"""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    index = {}
    for n, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or ":" not in line:
            continue
        key = line.split(":", 1)[0].strip()
        if key and not key.startswith("#"):
            index[key] = n
    return lines, index


def value_of(lines, index, key):
    if key not in index:
        return None
    raw = lines[index[key]].split(":", 1)[1]
    # 값 뒤 주석 제거. 값 자체에 '#' 가 들어갈 일은 없다.
    if "#" in raw:
        raw = raw.split("#", 1)[0]
    return raw.strip().strip('"').strip("'")


def read_csv_profile(path):
    out = {}
    with open(path, encoding="utf-8-sig", newline="") as f:
        for row in csv.reader(f):
            if not row or len(row) < 2:
                continue
            key, value = row[0].strip(), row[1].strip()
            if not key or key.lower() == "key" or key.startswith("#"):
                continue
            out[key] = value
    return out


def profiles():
    return sorted(f for f in os.listdir(HERE) if f.lower().endswith(".csv"))


def cmd_list():
    names = profiles()
    if not names:
        print("CSV 파일이 없다.")
        return 1
    print("사용 가능한 설정 (%s):" % HERE)
    for name in names:
        data = read_csv_profile(os.path.join(HERE, name))
        print("  %-20s %-8s %s:%s" % (name, data.get("name", "-"),
                                      data.get("gcs_host", "-"),
                                      data.get("telemetry_port", "-")))
    return 0


def cmd_show():
    lines, index = read_yaml(CONF)
    print("현재 설정 (%s):" % os.path.basename(CONF))
    for key in ("active", "description", "gcs_host", "telemetry_port",
                "relay_port", "ack_port", "pixhawk_device", "pixhawk_baud",
                "send_hz", "video_fps"):
        if key in index:
            print("  %-16s %s" % (key, value_of(lines, index, key)))
    return 0


def cmd_apply(csv_name):
    path = csv_name if os.path.isabs(csv_name) else os.path.join(HERE, csv_name)
    if not os.path.exists(path) and not path.lower().endswith(".csv"):
        path += ".csv"
    if not os.path.exists(path):
        print("CSV를 찾을 수 없다: %s" % path, file=sys.stderr)
        print("", file=sys.stderr)
        cmd_list()
        return 1

    data = read_csv_profile(path)
    if not data:
        print("CSV가 비어 있다: %s" % path, file=sys.stderr)
        return 1

    unknown = sorted(set(data) - ALLOWED)
    if unknown:
        print("모르는 항목이라 무시한다: %s" % ", ".join(unknown))
        for key in unknown:
            data.pop(key)

    # name 은 active 로 옮긴다. CSV 쪽 표기가 더 자연스러워 둘 다 받는다.
    if "name" in data:
        data.setdefault("active", data.pop("name"))
    else:
        data.setdefault("active", os.path.splitext(os.path.basename(path))[0])

    lines, index = read_yaml(CONF)
    changed, added, same = [], [], 0
    for key, new in data.items():
        old = value_of(lines, index, key)
        if key in index:
            if old == new:
                same += 1
                continue
            lines[index[key]] = "%s: %s" % (key, new)
            changed.append((key, old, new))
        else:
            lines.append("%s: %s" % (key, new))
            added.append((key, new))

    if not changed and not added:
        print("바뀐 것 없음. 이미 %s 설정이다." % data.get("active"))
        return 0

    with open(CONF, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    print("%s -> %s" % (os.path.basename(path), os.path.basename(CONF)))
    for key, old, new in changed:
        print("  %-16s %s  ->  %s" % (key, old, new))
    for key, new in added:
        print("  %-16s (추가)  ->  %s" % (key, new))
    if same:
        print("  %d개 항목은 이미 같았다." % same)
    print("")
    print("서비스에 반영하려면:  sudo systemctl restart gcs")
    return 0


def main(argv):
    if not os.path.exists(CONF):
        print("설정 파일이 없다: %s" % CONF, file=sys.stderr)
        return 1
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[1] == "--list":
        return cmd_list()
    if argv[1] == "--show":
        return cmd_show()
    return cmd_apply(argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
