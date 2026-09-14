# E220-900T22D LoRa 통신 테스트 정리

## 1. 테스트 목적

기존 인수인계에서는 ESP32 2대와 EBYTE E01-ML01DP5 2개를 이용해 다음 기능을 검증했다.

- 데이터 분할 전송
- ACK
- 재전송
- ECHO
- 오류 검출
- RTT 측정
- 거리 시험

현재는 최종 통신 모듈로 사용할 **E220-900T22D 2개**를 이용해 같은 방식의 통신 검증을 다시 수행하고 있다.

현재 구성은 다음과 같다.

```text
[Windows PC]
      │ USB
      ▼
DX-PJ15-V1.1
      │ UART
      ▼
E220 #1
  ))) LoRa 무선 (((
E220 #2
      │ UART3
      ▼
[Raspberry Pi 4]
```

---

## 2. 사용 장비

- Raspberry Pi 4
- EBYTE E220-900T22D 2개
- DX-PJ15-V1.1 USB-UART 컨버터
- E220용 안테나 2개
- 브레드보드
- 점퍼선
- Windows PC

---

## 3. 배선

### 3.1 Raspberry Pi 4 ↔ E220-900T22D

```text
Raspberry Pi 4                E220-900T22D
------------------------------------------------
Pin 7  / GPIO4  (TXD3)  ---> RXD
Pin 29 / GPIO5  (RXD3)  <--- TXD
Pin 1  / 3.3V            ---> VCC
Pin 39 / GND             ---> GND
Pin 34 / GND             ---> M0
Pin 30 / GND             ---> M1

AUX                         미사용
```

주의사항:

- TX와 RX는 서로 교차 연결한다.
  - Pi TX → E220 RXD
  - Pi RX ← E220 TXD
- M0, M1은 GND에 연결해 사용한다.
- AUX는 현재 테스트에서는 사용하지 않는다.
- Raspberry Pi 쪽 E220 전원은 3.3V를 사용했다.

---

### 3.2 DX-PJ15-V1.1 ↔ E220-900T22D

```text
DX-PJ15-V1.1                E220-900T22D
----------------------------------------
5V                   ---> VCC
GND                  ---> 브레드보드 (-)
                           ├─ GND
                           ├─ M0
                           └─ M1

TXD                  ---> RXD
RXD                  <--- TXD

AUX                       미사용
```

PJ15의 GND 하나를 브레드보드 `-` 레일에 연결한 뒤, 해당 레일을 E220의 GND, M0, M1에 공통으로 연결했다.

```text
PJ15 GND
   │
   ▼
브레드보드 (-)
   ├── E220 GND
   ├── E220 M0
   └── E220 M1
```

M0/M1을 연결하지 않고 floating 상태로 두었을 때 PuTTY에서 이상 문자가 출력되거나 통신이 불안정한 현상이 확인되었다.

현재 테스트에서는:

```text
M0 → GND
M1 → GND
```

상태로 사용한다.

---

## 4. Raspberry Pi UART 설정

기존 UART0 GPIO14/15(Pin 8/10)는 다른 장비에서 사용 중이므로 E220은 **UART3**를 사용한다.

UART3 GPIO:

```text
GPIO4 → TXD3
GPIO5 → RXD3
```

`/boot/firmware/config.txt`에 다음 내용을 추가했다.

```text
dtoverlay=uart3
```

설정 후 재부팅:

```bash
sudo reboot
```

재부팅 후 확인:

```bash
ls -l /dev/ttyAMA*
```

정상적으로 다음 장치가 생성되었다.

```text
/dev/ttyAMA3
```

현재 Raspberry Pi에서 E220 통신 포트는 다음과 같다.

```text
/dev/ttyAMA3
```

---

## 5. UART 설정

현재 테스트에서 사용하는 UART 설정:

```text
Baud rate    : 9600
Data bits    : 8
Parity       : None
Stop bits    : 1
Flow control : None
```

즉 9600 baud, 8N1 설정을 사용한다.

---

## 6. 기본 양방향 통신 확인

Raspberry Pi에서 다음 명령으로 UART 수신/송신을 확인했다.

```bash
python3 -m serial.tools.miniterm /dev/ttyAMA3 9600
```

PC → Raspberry Pi 방향으로 `HELLO` 전송에 성공했다.

반대로 Raspberry Pi → PC 방향도 정상적으로 수신됨을 확인했다.

따라서:

```text
PC
→ PJ15
→ E220 #1
→ LoRa
→ E220 #2
→ Raspberry Pi
```

및 역방향 통신이 모두 정상적으로 동작한다.

---

## 7. 통신 검증 구조

기존 E01 통신 시험 방식을 참고해 E220에서도 다음 기능을 구현했다.

- 데이터 분할 전송
- Session 번호
- Sequence 번호
- 전체 패킷 수
- 데이터 길이
- ACK
- ECHO
- Timeout 검출
- DATA mismatch 검출
- 실패 패킷 위치 확인
- 자동 재전송
- 패킷별 RTT 측정
- 전체 데이터 재조립
- 최종 원본 데이터 비교

논리적인 패킷 형식:

```text
DATA | session | sequence | totalPackets | dataLength | data
```

예:

```text
DATA|12345|5|24|23|ABCDEFGHIJKLMNOPQRSTUVW
```

Raspberry Pi는 DATA를 정상적으로 수신하면 ACK를 보낸다.

```text
ACK|12345|5
```

이후 받은 데이터를 그대로 ECHO한다.

```text
ECHO|12345|5|24|23|ABCDEFGHIJKLMNOPQRSTUVW
```

PC에서는 ACK 및 ECHO를 확인하고 원본 데이터와 비교한다.

---

## 8. 데이터 분할 전송

테스트 데이터는 총 540바이트를 사용했다.

한 패킷의 실제 데이터 크기는 23바이트로 설정했다.

```text
Original data : 540 bytes
Chunk size    : 23 bytes
Total packets : 24
```

즉 540바이트 데이터를 24개의 패킷으로 나눠 순서대로 전송한다.

각 패킷에는 sequence 번호가 있으므로 어떤 패킷이 실패했는지 확인할 수 있다.

---

## 9. 기본 분할 전송 테스트 결과

540바이트 데이터를 23바이트 단위로 분할해 총 24개 패킷을 송수신했다.

결과:

```text
========== RESULT ==========
Original bytes : 540
Returned bytes : 540
Packets        : 24
Passed packets : 24
Failed packets : 0
[PASS] Every byte returned correctly.
Average RTT    : 1213.10 ms
Minimum RTT    : 969.43 ms
Maximum RTT    : 1307.56 ms
Total time     : 29.13 s
```

확인 결과:

```text
데이터 분할       PASS
ACK               PASS
ECHO              PASS
Session 검사      PASS
Sequence 검사     PASS
Length 검사       PASS
DATA 비교         PASS
RTT 측정          PASS
전체 데이터 재조립 PASS
```

540바이트 전체가 원본과 동일하게 복구됨을 확인했다.

---

## 10. ACK와 ECHO

### ACK

ACK는 수신 측에서:

```text
"해당 패킷을 정상적으로 받았다"
```

는 의미로 보내는 응답이다.

예:

```text
PC → DATA seq=5
Pi → ACK seq=5
```

PC는 ACK가 도착하지 않으면 해당 패킷 전송에 실패했다고 판단한다.

### ECHO

ECHO는 받은 실제 데이터를 다시 송신 측으로 돌려보내는 방식이다.

```text
PC → HELLO
Pi → HELLO
```

PC는 자신이 보낸 데이터와 돌아온 데이터를 비교해 데이터가 변형되지 않았는지 확인한다.

---

## 11. 자동 재전송

ACK가 일정 시간 안에 도착하지 않으면 동일한 sequence 패킷을 다시 전송하도록 구현했다.

현재 설정:

```text
ACK_TIMEOUT  : 2초
ECHO_TIMEOUT : 2초
MAX_RETRIES  : 3회
```

동작 흐름:

```text
패킷 전송
   ↓
ACK 대기
   ↓
ACK 없음
   ↓
Timeout
   ↓
같은 sequence 재전송
   ↓
ACK 수신
   ↓
ECHO 확인
```

---

## 12. 강제 재전송 테스트

재전송 코드가 실제로 동작하는지 확인하기 위해 Raspberry Pi에서 `sequence=5`의 첫 번째 전송을 의도적으로 무시하도록 했다.

테스트 흐름:

```text
PC
 │
 │ seq=5 attempt=1
 ▼
Raspberry Pi
 │
 │ 첫 번째 패킷 의도적으로 무시
 │ ACK / ECHO 미전송
 ▼
PC
 │
 │ ACK Timeout
 │
 │ 자동 재전송
 ▼
seq=5 attempt=2
 │
 ▼
Raspberry Pi
 │
 │ ACK + ECHO
 ▼
PC
 │
 ▼
PASS
```

결과:

```text
========== RESULT ==========
Original bytes : 540
Returned bytes : 540
Packets        : 24
Passed packets : 24
Failed packets : 0
Retries        : 1
[PASS] Every byte returned correctly.
Average RTT    : 1217.20 ms
Minimum RTT    : 1112.25 ms
Maximum RTT    : 1302.84 ms
Total time     : 31.26 s
```

결과적으로 첫 번째 전송 실패 후 자동 재전송이 1회 발생했고, 최종적으로 모든 데이터를 정상 복구했다.

따라서:

```text
전송 실패 감지
→ ACK Timeout
→ 자동 재전송
→ ACK 수신
→ ECHO 수신
→ 원본 데이터 비교
→ 정상 복구
```

과정이 실제로 정상 동작함을 확인했다.

---

## 13. RTT

RTT(Round Trip Time)는 PC에서 패킷을 보낸 시점부터 Raspberry Pi가 데이터를 받은 뒤 다시 ECHO를 보내 PC로 돌아오기까지 걸리는 왕복시간이다.

```text
PC
 │ DATA
 ▼
Raspberry Pi
 │ ECHO
 ▼
PC
```

현재 테스트에서는 평균 약 1.2초의 RTT가 측정되었다.

이 값에는 단순 무선 전파 시간뿐만 아니라 다음 요소도 포함된다.

- PC Python 처리
- USB-UART 처리
- E220 내부 처리
- UART 송수신
- Raspberry Pi Python 처리
- ACK 및 ECHO 송수신

따라서 약 1.2초가 무선 신호 자체의 전파시간을 의미하는 것은 아니다.

---

## 14. Raspberry Pi 정상 Echo Server

거리시험에서는 강제로 패킷을 버리지 않는 원래 `echo_server.py`를 사용한다.

현재 코드:

```python
import serial

PORT = "/dev/ttyAMA3"
BAUD = 9600

ser = serial.Serial(PORT, BAUD, timeout=1)

print(f"[READY] E220 test server on {PORT} @ {BAUD}")

while True:
    try:
        line = ser.readline()

        if not line:
            continue

        try:
            text = line.decode("utf-8").strip()
        except UnicodeDecodeError:
            print("[ERROR] Invalid UTF-8")
            continue

        print("[RX]", text)

        parts = text.split("|")

        if len(parts) < 6:
            print("[ERROR] Invalid packet")
            continue

        packet_type = parts[0]

        if packet_type != "DATA":
            continue

        session = parts[1]
        sequence = parts[2]
        total = parts[3]
        length = parts[4]
        data = "|".join(parts[5:])

        # ACK 전송
        ack = f"ACK|{session}|{sequence}\n"
        ser.write(ack.encode())
        ser.flush()

        print("[ACK]", ack.strip())

        # 받은 DATA 그대로 ECHO
        echo = (
            f"ECHO|{session}|{sequence}|"
            f"{total}|{length}|{data}\n"
        )

        ser.write(echo.encode())
        ser.flush()

        print("[ECHO]", echo.strip())

    except KeyboardInterrupt:
        break

ser.close()
print("[STOP]")
```

실행:

```bash
python3 echo_server.py
```

---

## 15. 거리시험

현재 기능 검증은 완료했고 다음 단계는 **거리별 통신 성능 시험**이다.

예정 거리:

```text
1 m
5 m
10 m
20 m
50 m
100 m
...
```

가능한 범위까지 거리를 늘려가며 동일한 조건으로 시험한다.

거리시험 시 Raspberry Pi에서는:

```bash
python3 echo_server.py
```

를 실행한다.

PC에서는 저장해둔:

```bash
python lora_distance_test.py
```

를 실행한다.

실행 시 현재 시험 거리를 입력한다.

예:

```text
테스트 거리 입력 (예: 1m, 10m, 50m): 5m
```

각 거리에서 100개 패킷을 전송하고 결과를 기록한다.

---

## 16. 거리시험 측정 항목

각 거리에서 다음 값을 측정한다.

```text
거리
총 전송 패킷 수
성공 패킷 수
실패 패킷 수
패킷 손실률
재전송 횟수
Timeout 횟수
Data mismatch 횟수
평균 RTT
최소 RTT
최대 RTT
전체 시험시간
```

거리시험 결과는 PC에서 다음 CSV 파일에 누적 저장한다.

```text
lora_distance_results.csv
```

예상 형태:

```text
distance,session,sent,passed,failed,failure_rate_percent,retries,timeout_events,data_mismatch,avg_rtt_ms,min_rtt_ms,max_rtt_ms,total_time_s
1m,12543,100,100,0,0.00,0,0,0,1201.23,1102.34,1305.55,121.20
5m,39212,100,100,0,0.00,1,1,0,1215.61,1114.12,1402.91,124.53
10m,47122,100,98,2,2.00,8,10,0,1288.32,1121.81,1998.24,141.25
```

---

## 17. 거리시험 시 주의사항

거리만 바꾸고 나머지 조건은 최대한 동일하게 유지한다.

- 같은 코드 사용
- 같은 Baud rate 사용
- 같은 E220 설정 사용
- 같은 안테나 사용
- 같은 안테나 방향 유지
- 같은 전원 조건 유지
- 같은 패킷 수 사용

이렇게 해야 거리 변화에 따른 성능 차이를 비교할 수 있다.

또한 거리시험에서는 강제 재전송 테스트용 서버를 사용하면 안 된다.

```text
재전송 강제 시험
→ echo_server_retry_test.py

실제 거리 시험
→ echo_server.py
```

거리시험에서는 정상 `echo_server.py`를 사용한다.

---

## 18. 현재까지 검증 상태

- [x] Raspberry Pi ↔ E220 UART 연결
- [x] PC ↔ DX-PJ15-V1.1 ↔ E220 연결
- [x] E220 양방향 무선 통신
- [x] 데이터 분할 전송
- [x] Session 번호
- [x] Sequence 번호
- [x] 데이터 길이 확인
- [x] ACK
- [x] ECHO
- [x] Timeout 검출
- [x] DATA mismatch 검사
- [x] 패킷별 오류 위치 확인
- [x] 자동 재전송 로직
- [x] 강제 전송 실패 후 재전송 검증
- [x] RTT 측정
- [x] 전체 데이터 재조립
- [x] 540바이트 전체 데이터 일치 확인
- [ ] 1 m 거리시험
- [ ] 5 m 거리시험
- [ ] 10 m 이상 거리시험
- [ ] 거리별 패킷 손실률 비교
- [ ] 거리별 재전송 횟수 비교
- [ ] 거리별 RTT 비교

---

## 19. 현재 상태 요약

현재까지 E220-900T22D 2개를 이용한 기본 양방향 통신과 통신 프로토콜 테스트가 완료되었다.

검증 완료된 기능:

```text
데이터 분할
→ 패킷 번호 부여
→ LoRa 전송
→ ACK 확인
→ ACK 미수신 시 자동 재전송
→ ECHO 수신
→ 원본 데이터 비교
→ 데이터 재조립
→ RTT 측정
```

강제로 패킷 하나를 실패시킨 테스트에서도 자동 재전송을 통해 최종 540바이트 전체가 정상 복구되는 것을 확인했다.

현재 남은 주요 작업은 **실제 거리를 증가시키면서 패킷 손실률, 재전송 횟수, RTT 변화를 측정하는 거리시험**이다.
