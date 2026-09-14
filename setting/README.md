# setting

프로젝트 전역 설정 파일과, 그 설정을 읽는 공용 리더를 모아둔다.

- `port.yaml` — 장치 포트/보드레이트/송신 주기 등 통신 설정의 단일 출처.
- `cam_sets.yaml` — 카메라 녹화 설정의 단일 출처.
- `conf.py` — Python 쪽(ELRS/mav.py 등)에서 `port.yaml`을 읽는 공용 모듈.
- `conf.c` / `conf.h` — C 쪽에서 `port.yaml`을 읽는 공용 라이브러리. `conf.py`와 동일한 규칙을 따른다.
