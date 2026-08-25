import os
import pty
import struct
import subprocess
import time

def crc_accumulate(byte, crc):
    tmp = byte ^ (crc & 0xff)
    tmp ^= (tmp << 4) & 0xff
    return ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xffff

def frame(msgid, payload, extra, seq):
    header = bytes((len(payload), seq, 1, 1, msgid))
    crc = 0xffff
    for b in header + payload:
        crc = crc_accumulate(b, crc)
    crc = crc_accumulate(extra, crc)
    return b"\xfe" + header + payload + struct.pack("<H", crc)

master, slave = pty.openpty()
slave_name = os.ttyname(slave)
proc = subprocess.Popen(
    ["./data_test", slave_name, "115200"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=1,
)
attitude = struct.pack("<I6f", 1234, 0.1, -0.2, 1.2, 0.0, 0.0, 0.0)
global_position = struct.pack(
    "<IiiiihhhH", 1234, 374000000, 1270000000, 123450, 23450, 10, -20, 3, 9000
)
time.sleep(0.5)
attitude_frame = frame(30, attitude, 39, 1)
global_frame = frame(33, global_position, 104, 2)
print("frames", len(attitude_frame), len(global_frame), attitude_frame.hex())
try:
    from pymavlink import mavutil
    parser = mavutil.mavlink.MAVLink(None)
    for byte in attitude_frame + global_frame:
        message = parser.parse_char(bytes((byte,)))
        if message is not None:
            print("pymavlink", message.get_type())
except Exception as exc:
    print("pymavlink check failed", exc)
os.write(master, attitude_frame)
os.write(master, global_frame)
time.sleep(1.2)
proc.terminate()
try:
    output = proc.communicate(timeout=2)[0]
except subprocess.TimeoutExpired:
    proc.kill()
    output = proc.communicate()[0]
print(output)
