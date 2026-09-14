import serial

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)), "..", "setting"))
import conf

# setting/port.yaml 에서 읽는다. 아래는 그 파일이 없을 때의 기본값.
PORT = conf.get("lora_device", "/dev/ttyAMA3")
BAUD = conf.num("lora_baud", 9600)

ser = serial.Serial(PORT, BAUD, timeout=1)

print(f"[READY] E220 retry-test server on {PORT} @ {BAUD}")

# seq=5를 한 번만 일부러 버리기 위한 변수
dropped_once = False

try:
    while True:
        line = ser.readline()

        if not line:
            continue

        try:
            text = line.decode("utf-8").strip()
        except UnicodeDecodeError:
            print("[ERROR] Invalid UTF-8")
            continue

        print(f"[RX] {text}")

        parts = text.split("|")

        if len(parts) < 6:
            print("[ERROR] Invalid packet")
            continue

        packet_type = parts[0]

        if packet_type != "DATA":
            print(f"[IGNORE] Unknown packet type: {packet_type}")
            continue

        session = parts[1]
        sequence = parts[2]
        total = parts[3]
        length = parts[4]
        data = "|".join(parts[5:])

        # -------------------------------------------------
        # 강제 재전송 테스트
        # sequence=5 패킷을 처음 한 번만 일부러 무시
        # ACK도 안 보내고 ECHO도 안 보냄
        # -------------------------------------------------
        if sequence == "5" and not dropped_once:
            print("[TEST DROP] seq=5 first attempt intentionally ignored")
            dropped_once = True
            continue

        # ACK 전송
        ack = f"ACK|{session}|{sequence}\n"

        ser.write(ack.encode("utf-8"))
        ser.flush()

        print(f"[ACK] {ack.strip()}")

        # ECHO 전송
        echo = (
            f"ECHO|{session}|{sequence}|"
            f"{total}|{length}|{data}\n"
        )

        ser.write(echo.encode("utf-8"))
        ser.flush()

        print(f"[ECHO] {echo.strip()}")

except KeyboardInterrupt:
    print("\n[STOP]")

finally:
    ser.close()
