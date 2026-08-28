# gimbal

Pixhawk 의 자세(pitch/roll)를 MAVLink 로 받아 서보 두 개를 실시간으로 돌린다.
카메라가 아래를 보도록 유지하는 것이 목적이다.

## 폴더 구조

```text
camera/gimbal/
├── gimbal_full.c      운영용 본체. record.sh 가 이것을 쓴다
├── zero_calib.c       영점 맞추는 대화형 도구
├── zero_point.h       영점 읽기/쓰기 공용 헬퍼
├── zero_point.yaml    영점 값 (실제 설정 파일)
├── mav_serial.h       시리얼 열기 공용 헬퍼
├── bin/               빌드 결과가 전부 여기 들어간다
├── split/             pitch/roll 을 따로 돌리던 구버전
└── test/              MAVLink 없이 서보만 돌려본 실험
```

## 사용법

### bin/gimbal_full

```sh
./bin/gimbal_full                  # 기본 /dev/ttyACM0
./bin/gimbal_full /dev/ttyACM1     # 포트 지정
GIMBAL_SERIAL=/dev/ttyACM1 ./bin/gimbal_full
```

pitch/roll 서보를 한 프로세스에서 함께 구동한다. Pixhawk 의 heartbeat 를
기다렸다가 `SET_MESSAGE_INTERVAL` 로 ATTITUDE 스트림을 20 Hz 로 요청하고,
이후 들어오는 자세값을 계속 반영한다. `Ctrl+C` 로 멈춘다.

**보통 직접 실행하지 않는다.** `scripts/record.sh` 가 촬영 시간만큼 이것을
띄웠다가 정리한다.

### bin/zero_calib

```sh
./bin/zero_calib
```

서보의 영점을 눈으로 보며 맞춘다. 실행하면 `zero_point.yaml` 의 현재 값으로
서보를 옮긴 뒤 키 입력을 기다린다.

| 키 | 동작 |
|----|------|
| `w` / `s` | pitch 조정 (±1) |
| `a` / `d` | roll 조정 (±1) |
| `space` | 현재 값을 `zero_point.yaml` 에 저장 |
| `q` | 저장하지 않고 종료 |

### split/gimbal — 구버전

```sh
./split/gimbal
```

`pitch` 와 `roll` 을 별도 프로세스로 동시에 띄운다. `Ctrl+C` 로 둘 다 멈춘다.

> **권장하지 않는다.** 두 프로세스가 각자 `/dev/ttyACM0` 을 여는데, 같은
> 시리얼을 둘이 읽으면 MAVLink 바이트를 서로 나눠 가져 양쪽 파싱이 불안정해진다.
> `bin/gimbal_full` 이 같은 일을 한 프로세스로 한다.
> `split/pitch.c` 와 `roll.c` 는 포트가 `/dev/ttyACM0` 으로 박혀 있다.

### test/ — 서보 단독 확인

MAVLink 를 쓰지 않고 서보 배선만 확인하는 실험 코드다. 빌드하면 `bin/` 에 들어간다.

| 파일 | 하는 일 |
|------|---------|
| `servo_test.c` | pitch 서보(핀 1)를 duty 5 ↔ 25 왕복. 가장 단순 |
| `servo2_test.c` | roll 서보(핀 26)를 15 → 10 → 20 → 15 왕복 |
| `serve_test_gpt.c` | pitch 서보를 중앙 → 양끝 → 중앙 (초기 실험 코드) |
| `gimbal_manual.c` | 터미널에 `<pan> <tilt>`(0~180)를 입력해 수동 구동 |

## 하드웨어

| 서보 | wiringPi 핀 | GPIO | 물리 핀 |
|------|-------------|------|---------|
| pitch | 1 | GPIO18 | 12 |
| roll | 26 | GPIO12 | 32 |

softPWM range 는 200 이고, duty 10~20 이 펄스폭 1.0~2.0 ms(서보 표준 가동
범위)에 대응한다. 15 가 정중앙(1.5 ms)이다.

구동 시에는 영점을 중심으로 duty ±10 까지 움직이되, 절대 한계 5~25
(0.5~2.5 ms)로 클램프된다. `servo_test.c` 로 왕복 검증된 값이라 영점이 어디에
있든 이 범위를 벗어나지 않는다.

## 영점

`zero_point.yaml` 이 실제 설정 파일이고, `zero_point.h` 가 그것을 읽고 쓴다.
`gimbal_full`, `pitch`, `roll` 은 시작할 때 이 값으로 먼저 이동한 뒤, 그 값을
중심으로 자세만큼 duty 를 계산한다.

경로는 `zero_point.h` 에 박혀 있다.

```c
#define CUBESAT_ROOT_DEFAULT "/home/jih/cubesat"
```

예전에는 `"zero_point.yaml"` 을 상대경로로 열었는데, 그러면 짐벌 폴더 밖에서
실행할 때 파일을 못 찾고 **조용히 기본값 15 로 떨어졌다**. `scripts/record.sh`
는 `scripts/` 에서 부르므로 실제로 그랬다. 지금은 절대경로라 어디서 실행하든
같은 파일을 본다. 다른 계정이나 다른 위치에 두면 `CUBESAT_ROOT` 로 덮어쓴다.

## 빌드

결과물은 전부 `bin/` 에 넣는다. 짐벌 폴더에서 실행한다.

```sh
# 운영용
gcc -Wall -I. -I ~/.local/include -L ~/.local/lib \
    gimbal_full.c -o bin/gimbal_full -lwiringPi -lpthread

# 영점 도구
gcc -Wall -I. -I ~/.local/include -L ~/.local/lib \
    zero_calib.c -o bin/zero_calib -lwiringPi -lpthread

# split/ 과 test/ 안의 소스도 같은 형태
gcc -Wall -I. -I ~/.local/include -L ~/.local/lib \
    split/pitch.c -o bin/pitch -lwiringPi -lpthread
```

`-I.` 이 필요한 이유는 `mav_serial.h` 와 `zero_point.h` 가 짐벌 루트에 있고
`split/`, `test/` 안의 소스가 그것을 include 하기 때문이다.

wiringPi 와 MAVLink 헤더는 `~/.local/include`, 라이브러리는 `~/.local/lib` 에
설치되어 있다.

`gimbal_full` 만 다시 빌드할 거라면 `scripts/record.sh build` 로도 된다.

## 시리얼 포트

Pixhawk 는 MAVLink 포트를 두 개 노출한다.

| 포트 | 쓰는 곳 |
|------|---------|
| `/dev/ttyACM0` | `gcs` (텔레메트리 송신) |
| `/dev/ttyACM1` | 짐벌 — `record.sh` 가 이렇게 부른다 |

한 장치를 두 프로세스가 열면 바이트를 나눠 가져 양쪽 파싱이 모두 깨진다.
그래서 짐벌과 텔레메트리를 동시에 돌리려면 포트를 반드시 나눠야 한다.
`cubesat.sh` 가 그 전제 위에서 둘을 함께 띄운다.
