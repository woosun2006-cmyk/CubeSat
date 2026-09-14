import time

import serial


import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)), "..", "setting"))
import conf

# setting/port.yaml 에서 읽는다. 아래는 그 파일이 없을 때의 기본값.
PORT = conf.get("crsf_device", "/dev/serial0")
BAUD = conf.num("crsf_baud", 420000)
CRSF_ADDRESS_FLIGHT_CONTROLLER = 0xC8
CRSF_ADDRESS_RADIO_TRANSMITTER = 0xEA
CRSF_ADDRESS_CRSF_RECEIVER = 0xEC
CRSF_FRAMETYPE_RC_CHANNELS_PACKED = 0x16


def crc8_dvb_s2(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0xD5) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def unpack_channels(payload):
    value = int.from_bytes(payload[:22], "little")
    return [(value >> (11 * index)) & 0x7FF for index in range(16)]


def read_frames(port=PORT, baud=BAUD):
    valid_addresses = {
        CRSF_ADDRESS_FLIGHT_CONTROLLER,
        CRSF_ADDRESS_RADIO_TRANSMITTER,
        CRSF_ADDRESS_CRSF_RECEIVER,
        0xEE,
        0xEF,
    }
    buffer = bytearray()

    with serial.Serial(port, baud, timeout=0.05) as ser:
        while True:
            data = ser.read(256)
            if data:
                buffer.extend(data)

            while len(buffer) >= 4:
                address = buffer[0]
                if address not in valid_addresses:
                    del buffer[0]
                    continue

                length = buffer[1]
                if length < 2 or length > 62:
                    del buffer[0]
                    continue

                frame_length = length + 2
                if len(buffer) < frame_length:
                    break

                frame = bytes(buffer[:frame_length])
                del buffer[:frame_length]

                frame_type = frame[2]
                payload = frame[3:-1]
                crc = frame[-1]
                if crc8_dvb_s2(frame[2:-1]) != crc:
                    continue

                yield address, frame_type, payload


def main():
    print(f"Listening for CRSF on {PORT} @ {BAUD} baud")
    print("Move Pocket sticks. RC frames should print as ch1-ch8 values.")

    last_print = 0
    frame_count = 0
    start = time.time()
    for address, frame_type, payload in read_frames():
        frame_count += 1
        now = time.time()
        if frame_type == CRSF_FRAMETYPE_RC_CHANNELS_PACKED and len(payload) >= 22:
            channels = unpack_channels(payload)
            print(
                "RC "
                + " ".join(f"ch{i + 1}={channels[i]:4d}" for i in range(8)),
                flush=True,
            )
        elif now - last_print >= 2:
            last_print = now
            elapsed = max(now - start, 0.001)
            print(
                f"frames={frame_count} rate={frame_count / elapsed:.1f}/s "
                f"last_type=0x{frame_type:02X} from=0x{address:02X}",
                flush=True,
            )


if __name__ == "__main__":
    main()
