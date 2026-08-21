# CubeSat Telemetry

라즈베리파이에서 MAVLink 텔레메트리를 생성해 **ExpressLRS 무선 링크**를 통해 지상국(QGroundControl)으로 전송하는 테스트 시스템.

현재는 실제 비행 컨트롤러 없이 파이가 직접 MAVLink 메시지를 만들어 보내는 **송신 테스트 단계**다.

---

## 통신 경로

```
Raspberry Pi 4
    │  UART  /dev/serial0 (ttyS0) @ 460800 baud
    ▼
XR1 ELRS 수신기
    │  ExpressLRS 무선
    ▼
Pocket 조종기 / TX Backpack
    │  WiFi UDP
    ▼
노트북 / QGroundControl
```

### 각 구간 설정

**파이 UART**

`config.txt`에 `enable_uart=1` 필요. `/dev/serial0`은 Pi 4에서 `ttyS0`(mini UART)로 연결된다.

**Pocket 조종기**

| 항목 | 값 |
|---|---|
| Internal RF | `CRSF` |
| External RF | `OFF` |
| Link Mode | `MAVLink` |
| Other Devices → XR1 → Serial Protocol | `MAVLink` |
| Backpack → Telemetry | `WiFi` |

> Internal RF를 `CRSF`로 두어야 Pocket 내부 ELRS가 동작하고 `SYS → ExpressLRS` 메뉴가 정상적으로 열린다.

**TX Backpack (WiFi)**

| 항목 | 값 |
|---|---|
| Backpack Web UI | `http://10.0.0.1` |
| GCS IP Addresses | `10.0.0.100` |
| Send Port | `14550` |
| Listen Port | `14555` |

노트북이 `ExpressLRS Backpack` WiFi에 접속하면 `10.0.0.100`을 할당받고, Backpack이 이를 지상국으로 인식한다.

---

## 구성 파일

```
cubesat/
├── mav_test.py            MAVLink 텔레메트리 송신 (메인)
├── loopback_jih.py        UART 루프백 점검 도구
├── mavlink-test.service   systemd 유닛 원본
├── mavenv/                Python 가상환경 (git 제외)
└── .gitignore
```

### `mav_test.py`

`/dev/serial0`을 460800 baud로 열고 `source_system=1`, `source_component=1`로 송신한다.

| 주기 | 메시지 |
|---|---|
| 1초 | `HEARTBEAT` (QUADROTOR / ARDUPILOTMEGA / MAV_STATE_ACTIVE) |
| 1초 | `GPS_RAW_INT`, `GLOBAL_POSITION_INT` |
| 2초 | `NAMED_VALUE_FLOAT` / `NAMED_VALUE_INT` — `cpu_temp`, `sd_free`, `uptime_s`, `cam_count`, `cam_state` |
| 10초 | `STATUSTEXT` — `"CubeSat telemetry alive; cams=5; storage ok"` |

`cpu_temp`는 `/sys/class/thermal/thermal_zone0/temp`, `sd_free`는 루트 파티션 여유 공간(MB)에서 읽어 온다.

좌표는 **고정 테스트값**이다 (위도 37.5665, 경도 126.9780, 고도 100 m — 서울). GPS 모듈을 붙이면 이 부분을 실측값으로 교체해야 한다.

수신 측에서 들어오는 메시지는 `recv_match(blocking=False)`로 폴링해 표준 출력에 기록한다.

### `loopback_jih.py`

UART 배선 점검용. `/dev/serial0`을 **9600 baud**로 열고 `loopback test 0~4`를 쓴 뒤 되돌아오는 값을 읽어 출력한다. TX-RX를 직결한 상태에서 보낸 문자열이 그대로 돌아오면 UART 자체는 정상.

---

## 환경 구성

Python 3.13.5 기반 venv를 사용한다.

| 패키지 | 버전 |
|---|---|
| pymavlink | 2.4.49 |
| pyserial | 3.5 |

`mavenv/`는 `.gitignore`에 포함되어 저장소에 올라가지 않는다. 새 환경에서는 다음과 같이 만든다.

```bash
python3 -m venv --copies mavenv
./mavenv/bin/pip install pymavlink pyserial
```

> venv에는 절대경로가 박혀 있으므로 디렉터리를 옮기면 그대로 쓸 수 없다. 옮겼다면 텍스트 파일의 경로만 치환해야 한다. `bin/python`은 실제 ELF 바이너리이므로 `sed`를 걸면 안 된다.
>
> ```bash
> grep -rlI '<옛경로>/mavenv' mavenv | xargs -r sed -i 's|<옛경로>/mavenv|<새경로>/mavenv|g'
> ```

---

## 실행

### 서비스로 실행 (기본)

`mavlink-test.service`가 `enabled` 상태이며 부팅 시 자동 시작된다. `Restart=always`, `RestartSec=3`으로 비정상 종료 시 3초 후 재시작한다.

```bash
sudo cp mavlink-test.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mavlink-test.service
```

### 상태 확인

```bash
systemctl status mavlink-test.service
journalctl -u mavlink-test.service -f
```

정상이면 로그에 다음 두 줄이 1초 간격으로 번갈아 나타난다.

```
sent MAVLink heartbeat/gps/status
received from GCS: RADIO_STATUS RADIO_STATUS {rssi : 0, ...}
```

### 루프백 점검

`loopback_jih.py`는 같은 `/dev/serial0`을 다른 baud로 열기 때문에 서비스와 충돌한다. 반드시 서비스를 멈추고 실행한다.

```bash
sudo systemctl stop mavlink-test.service
./mavenv/bin/python loopback_jih.py
sudo systemctl start mavlink-test.service
```

---

## 동작 확인 방법

### 파이 쪽

`journalctl`에서 20초 동안의 송수신 횟수를 세면 1Hz 동작을 확인할 수 있다.

```bash
journalctl -u mavlink-test.service --since '20 seconds ago' | grep -c 'sent MAVLink'
journalctl -u mavlink-test.service --since '20 seconds ago' | grep -c 'received from GCS'
```

두 값이 모두 20 내외면 송신과 수신이 함께 동작하는 상태다. `received from GCS`가 들어온다는 것은 XR1이 전원을 받고 Serial Protocol이 MAVLink로 잡혀 UART 양방향이 성립했다는 뜻이다.

### 지상국 쪽

노트북을 `ExpressLRS Backpack` WiFi에 접속한 뒤 확인한다.

- `http://10.0.0.1` 에서 **Packets Downlink** 증가 → 파이 방향 데이터 수신 중
- **Packets Uplink** 증가 → 지상국 방향 데이터 전달 중
- **GCS IP Addresses** 에 `10.0.0.100` 표시 → 노트북이 지상국으로 인식됨

UDP 포트로 직접 확인하려면:

```bash
python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('0.0.0.0',14550));s.settimeout(10);[print(len(s.recv(2048)),'bytes') for _ in range(10)]"
```

QGroundControl에서는 UDP 14550으로 연결하면 `HEARTBEAT`, `GPS_RAW_INT`, `GLOBAL_POSITION_INT`와 `cpu_temp` / `sd_free` / `uptime_s` 값이 표시된다.

---

## 현재 상태

**확인된 것**

- 서비스가 `enabled` + `running` 상태로 부팅 시 자동 실행됨
- 송신 · 수신 모두 1Hz로 동작 (20초 측정에서 각 20회)
- UART 460800 baud 설정이 스크립트와 일치
- XR1로부터 `RADIO_STATUS`가 되돌아옴 → `Pi ↔ XR1` UART 양방향 성립
- QGroundControl에서 `HEARTBEAT`, `GPS_RAW_INT`, `GLOBAL_POSITION_INT`, `cpu_temp`, `sd_free`, `uptime_s` 수신 확인

**미해결**

- `RADIO_STATUS`의 `rssi` / `remrssi`가 계속 `0`으로 보고된다. ELRS Backpack이 해당 필드를 채우지 않는 것인지, RF 링크가 실제로 성립하지 않은 것인지 아직 구분하지 못했다. Pocket을 켠 상태에서 Backpack Web UI의 패킷 카운터와 함께 비교해야 판별할 수 있다.

---

## 알려진 제약

- **로그 기록량** — `mav_test.py`가 매 루프마다 `print`를 호출하고 journald가 이를 모두 기록하므로 SD 카드에 초당 약 2줄이 계속 쌓인다. 장기 운용 시 `journald.conf`의 `SystemMaxUse`로 상한을 두거나 출력 빈도를 줄이는 편이 좋다.
- **고정 좌표** — 위치 데이터는 실제 측정값이 아닌 하드코딩된 값이다.
- **포트 경합** — `/dev/serial0`을 여는 스크립트는 동시에 하나만 실행할 수 있다.
