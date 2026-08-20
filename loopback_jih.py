import serial
import time

PORT = "/dev/serial0"
BAUD = 9600

ser = serial.Serial(PORT, BAUD, timeout=1)

for i in range(5):
    msg = f"loopback test {i}\n"
    ser.write(msg.encode("utf-8"))
    time.sleep(0.2)
    data = ser.readline().decode("utf-8", errors="replace").strip()
    print(f"sent={msg.strip()} received={data}")
    time.sleep(0.5)

ser.close()
