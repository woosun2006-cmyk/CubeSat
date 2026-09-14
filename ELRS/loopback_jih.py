import serial
import time

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)), "..", "setting"))
import conf

# setting/port.yaml 에서 읽는다. 아래는 그 파일이 없을 때의 기본값.
PORT = conf.get("loopback_device", "/dev/serial0")
BAUD = conf.num("loopback_baud", 9600)

ser = serial.Serial(PORT, BAUD, timeout=1)

for i in range(5):
    msg = f"loopback test {i}\n"
    ser.write(msg.encode("utf-8"))
    time.sleep(0.2)
    data = ser.readline().decode("utf-8", errors="replace").strip()
    print(f"sent={msg.strip()} received={data}")
    time.sleep(0.5)

ser.close()
