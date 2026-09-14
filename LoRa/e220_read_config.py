import serial
import time

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)), "..", "setting"))
import conf

# setting/port.yaml 에서 읽는다. 아래는 그 파일이 없을 때의 기본값.
PORT = conf.get("lora_device", "/dev/ttyAMA3")
BAUD = conf.num("lora_baud", 9600)

ser = serial.Serial(
    PORT,
    BAUD,
    bytesize=8,
    parity="N",
    stopbits=1,
    timeout=2
)

time.sleep(0.5)
ser.reset_input_buffer()

# E220 레지스터 0x00부터 8바이트 읽기
cmd = bytes([0xC1, 0x00, 0x08])

print("TX:", cmd.hex(" "))

ser.write(cmd)
ser.flush()

time.sleep(0.5)

response = ser.read(32)

print("RX:", response.hex(" "))

ser.close()
