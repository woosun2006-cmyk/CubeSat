"""setting/port.yaml 한 곳에서 통신 설정을 읽는다.

conf.sh 와 같은 규칙으로 한 단짜리 "key: value" 만 본다. 파이에 PyYAML 을
더 넣지 않으려는 것이고, 파일이나 항목이 없으면 부르는 쪽의 기본값으로
조용히 돌아간다 -- 설정 파일이 없다고 텔레메트리가 멈추면 안 된다.
"""

import os

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "port.yaml")


def get(name, default=None):
    try:
        with open(PATH, "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.split("#", 1)[0].strip()
                if not line or ":" not in line:
                    continue
                key, _, value = line.partition(":")
                if key.strip() == name and value.strip():
                    return value.strip()
    except OSError:
        pass
    return default


def num(name, default):
    """기본값과 같은 형(int/float)으로 돌려준다."""
    try:
        return type(default)(get(name))
    except (TypeError, ValueError):
        return default
