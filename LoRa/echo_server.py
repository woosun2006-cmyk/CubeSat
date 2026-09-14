import serial

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)), "..", "setting"))
import conf

# setting/port.yaml 에서 읽는다. 아래는 그 파일이 없을 때의 기본값.
PORT = conf.get("lora_device", "/dev/ttyAMA3")
BAUD = conf.num("lora_baud", 9600)

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

        # ACK 먼저 전송
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
