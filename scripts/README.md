## 각 스크립트 사용법

### gcs.sh — 세 링크 한꺼번에
`./gcs.sh`
LTE + LoRa + ELRS 로 동시에 내보내고 파이 화면에 GUI 를 띄운다.
`./gcs.sh 10`
LTE 만 10 Hz 로 올린다. LoRa 와 ELRS 는 자기 주기를 지킨다.
`./gcs.sh nogui`
GUI 없이. ssh 로 띄워두고 나갈 때.
`./gcs.sh status` 설정 확인 / `./gcs.sh build` 빌드

### gcs_LTE.sh / gcs_LoRa.sh / gcs_ELRS.sh — 링크 하나씩
한 링크만 따로 시험할 때 쓴다. **세 개를 동시에 띄우면 안 된다** — 같은
픽스호크 포트와 같은 LoRa UART 를 서로 뺏는다. 셋을 같이 돌리려면 `gcs.sh`.

`./gcs_LTE.sh` LTE(UDP/VPN) + 파이 GUI
`./gcs_LoRa.sh` LoRa + LTE + GUI (`nogui`, `solo` 모드 있음)
`./gcs_ELRS.sh` ELRS 송신. `monitor`(CRSF 프레임), `loopback`(UART 배선),
`probe`(ATTITUDE 수신율) 로 링크 점검도 한다.

### record.sh
`./record.sh 10min`
10분동안 아래 방향의 카메라 촬영 기록 및 짐벌 작동
`./record.sh 10sec`
10초동안 아래방향의 카메라 촬영 기록 및 짐벌 작동
** 짐벌 작동은 촬영 시간 만큼 작동함 **

### cubesat.sh
위의 두 스크립트 합친 결과물(전체 미션 수행 스크립트)
`./cubesat.sh --50Hz --10min`
50Hz로 gcs 통신 및 10분동안 카메라 촬영 기록 및 짐벌 작동
** 50Hz, 10min  순서 바뀌어도 무관 **

## 링크별 주사율

인자로 주는 주사율은 **LTE 와 온보드 로그에만** 걸린다.

| 링크 | 주기 | 정하는 곳 |
|---|---|---|
| LTE | 인자 또는 `send_hz` | `gcs/connection/connecting_port.yaml` |
| LoRa | `lora_send_interval` (기본 1 Hz) | `setting/port.yaml` |
| ELRS | mav.py 자기 루프 (1 Hz) | `ELRS/mav.py` |

LoRa 를 1 Hz 위로 올리지 말 것. 한 줄이 262 B 인데 E220 기본 에어레이트
2.4 kbps 로는 한 줄 내보내는 데만 0.87 초가 든다. 더 빨리 보내야 하면 주기가
아니라 모듈의 에어레이트를 먼저 올린다.

## 장치 배분

한 시리얼 포트를 두 프로그램이 열면 바이트를 나눠 가져 양쪽 파싱이 모두
깨진다. 그래서 프로그램마다 포트가 갈려 있다.

| 장치 | 쓰는 쪽 |
|---|---|
| `ttyACM0` (`-if00`) | `ELRS/mav.py` |
| `ttyACM1` (`-if02`) | `gcs` (LTE/LoRa) |
| `/dev/serial0` | ELRS 백팩 송신 |
| `/dev/ttyAMA3` | LoRa (E220) |

**주의 — 짐벌과 gcs 가 같은 포트다.** `record.sh` 의 `GIMBAL_SERIAL` 이
`/dev/ttyACM1` 인데, `connecting_port.yaml` 의 `pixhawk_device` 도 `-if02`
(= `ttyACM1`) 로 바뀌었다. 그래서 `cubesat.sh` 처럼 gcs 와 짐벌을 같이 돌리면
둘 다 깨진다. 픽스호크가 내놓는 MAVLink 포트가 둘뿐인데 쓰려는 쪽이 셋
(mav.py / gcs / 짐벌)이라 생긴 문제다. 셋을 같이 돌리려면 한 프로세스가
픽스호크를 열고 나머지에 복제해 주는 중계가 필요하다.

## 로그 저장 경로
cubesat/log/gcs : gcs.sh 에서 보내는 값들에 대한 로그 저장
cubesat/log/video : record.sh 로 녹화한 영상 저장
