"""E220-900T22D 레지스터 설정 도구 — UART 보드레이트와 공중 전송률을 바꾼다.

왜 필요한가
-----------
LoRa 를 10 Hz 로 보내려면 46 B 프레임 하나가 100 ms 안에 공중을 건너야 한다.
공장 기본값인 공중 전송률 2.4 kbps 로는 한 프레임이 153 ms 라 물리적으로
불가능하다. 그 값은 모듈 레지스터에 있고, 레지스터는 **설정 모드**에서만
쓸 수 있다.

    공중 전송률   46 B 에어타임   10 Hz(100 ms) duty
    ------------------------------------------------
    2.4 k (현재)      153 ms      불가능
    4.8 k              77 ms      77 %  빠듯
    9.6 k              38 ms      38 %  가능
    19.2 k             19 ms      19 %  여유  <- 기본값
    38.4 k            9.6 ms      10 %
    62.5 k            5.9 ms       6 %  (모듈 최대)

빠를수록 수신 감도가 떨어져 통달거리가 줄어든다. 2.4k -> 19.2k 는 대략
9 dB 손해로, 거리로는 3분의 1 수준이다. 거리가 더 중요하면 9.6k 를 쓸 것.

배선 (먼저 해야 한다)
---------------------
지금 M0 과 M1 은 GND 에 점퍼로 물려 있다(README 3.1). 그 상태로는 모듈이
항상 일반 전송 모드라 레지스터를 쓸 수 없다. 점퍼 두 개를 GPIO 로 옮긴다.

    E220 M0 : Pin 34 (GND)  ->  Pin 31 (GPIO6)
    E220 M1 : Pin 30 (GND)  ->  Pin 33 (GPIO13)

옮긴 뒤에도 평소 동작은 그대로다 -- 이 스크립트가 끝나면서 두 핀을 0 으로
되돌려 놓기 때문에 GND 에 묶여 있던 것과 같은 상태가 된다.

E220 의 모드표는 E32 계열과 다르다. E32 는 설정 모드가 M0=0/M1=1 이지만
E220 은 **둘 다 HIGH** 다. E32 규칙을 쓰면 WOR 수신 모드로 들어가 버려서
레지스터 명령에 아무 응답이 없다.

    M0=0 M1=0   일반 전송 (평소)
    M0=1 M1=0   WOR 송신
    M0=0 M1=1   WOR 수신
    M0=1 M1=1   설정 모드 (이 스크립트가 잠깐 쓴다)

쓰는 법
-------
    python3 e220_configure.py --show                 지금 값만 읽기
    python3 e220_configure.py --air 19.2k --uart 19200
    python3 e220_configure.py --air 9.6k --uart 9600 --dry-run

바꾼 뒤에는 setting/port.yaml 의 lora_baud 를 --uart 와 같은 값으로 맞춰야
한다. 안 맞으면 파이와 모듈이 서로 다른 속도로 말하게 된다.

주의: 설정 모드에서 모듈은 **항상 9600 8N1** 로 대화한다. 설정한 UART
속도와는 무관하다. 그래서 아래 코드는 설정 중에는 9600 을 쓴다.
"""

import argparse
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial 이 없다:  python3 -m pip install pyserial", file=sys.stderr)
    raise SystemExit(1)

DEVICE = "/dev/ttyAMA3"
CONFIG_BAUD = 9600          # 설정 모드는 속도가 고정이다
M0_GPIO = 6                 # Pin 31
M1_GPIO = 13                # Pin 33

# REG0 비트 7-5
UART_CODES = {1200: 0, 2400: 1, 4800: 2, 9600: 3,
              19200: 4, 38400: 5, 57600: 6, 115200: 7}
# REG0 비트 2-0
AIR_CODES = {"2.4k": 0b010, "4.8k": 0b011, "9.6k": 0b100,
             "19.2k": 0b101, "38.4k": 0b110, "62.5k": 0b111}
AIR_NAMES = {v: k for k, v in AIR_CODES.items()}
AIR_NAMES[0b000] = "2.4k"
AIR_NAMES[0b001] = "2.4k"
UART_NAMES = {v: k for k, v in UART_CODES.items()}


def pin(gpio, level):
    """pinctrl 로 한 핀을 0/1 로. gpiozero 는 시스템 파이썬에 없고 lgpio 는
    venv 에 없어서, 어느 파이썬으로 돌려도 되는 CLI 쪽을 쓴다."""
    subprocess.run(["pinctrl", "set", str(gpio), "op", "dh" if level else "dl"],
                   check=True, capture_output=True)


def set_mode(config):
    """config=True 면 설정 모드(M0=1,M1=1), 아니면 일반 전송(0,0).

    E220 은 설정 모드가 두 핀 모두 HIGH 다 -- E32 의 M0=0/M1=1 과 다르다.
    """
    level = 1 if config else 0
    pin(M0_GPIO, level)
    pin(M1_GPIO, level)
    # 모드 전환 뒤 AUX 가 안정될 때까지. 데이터시트상 수 ms 면 되지만
    # AUX 를 안 물려 놨으므로 넉넉히 준다.
    time.sleep(0.2)


def read_registers(ser):
    """0x00 부터 8 바이트. 실패하면 None."""
    ser.reset_input_buffer()
    ser.write(bytes([0xC1, 0x00, 0x08]))
    ser.flush()
    time.sleep(0.3)
    reply = ser.read(11)
    # 정상 응답은 C1 00 08 뒤에 레지스터 8 바이트
    if len(reply) < 11 or reply[0] != 0xC1:
        return None
    return reply[3:11]


def describe(regs):
    reg0 = regs[2]
    uart = UART_NAMES.get((reg0 >> 5) & 0x07, "?")
    air = AIR_NAMES.get(reg0 & 0x07, "?")
    parity = (reg0 >> 3) & 0x03
    return (f"  주소   : {regs[0]:02X}{regs[1]:02X}\n"
            f"  REG0   : 0x{reg0:02X}  -> UART {uart} bps / 패리티 {parity} / 공중 {air} bps\n"
            f"  REG1   : 0x{regs[3]:02X}\n"
            f"  채널   : {regs[4]}  ({850.125 + regs[4]:.3f} MHz)\n"
            f"  REG3   : 0x{regs[5]:02X}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="E220 UART/공중 전송률 설정")
    ap.add_argument("--device", default=DEVICE)
    ap.add_argument("--air", choices=sorted(AIR_CODES), default="19.2k",
                    help="공중 전송률 (기본 19.2k -- 10 Hz 에 여유 있음)")
    ap.add_argument("--uart", type=int, choices=sorted(UART_CODES), default=19200,
                    help="모듈 UART 보드레이트 (기본 19200)")
    ap.add_argument("--show", action="store_true", help="읽기만 하고 끝")
    ap.add_argument("--dry-run", action="store_true", help="쓸 값만 보여주고 끝")
    args = ap.parse_args(argv)

    try:
        set_mode(config=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f"GPIO 를 못 건드린다 ({exc}). M0/M1 배선을 먼저 옮길 것.",
              file=sys.stderr)
        return 1

    try:
        with serial.Serial(args.device, CONFIG_BAUD, timeout=1) as ser:
            before = read_registers(ser)
            if before is None:
                print("모듈이 설정 모드에서 응답하지 않는다.", file=sys.stderr)
                print("M0->GPIO6(Pin31), M1->GPIO13(Pin33) 배선을 확인할 것.",
                      file=sys.stderr)
                return 1

            print("현재 설정:")
            print(describe(before))

            if args.show:
                return 0

            reg0 = (UART_CODES[args.uart] << 5) | (before[2] & 0x18) | AIR_CODES[args.air]
            print(f"\n쓸 값: REG0 = 0x{reg0:02X}  (UART {args.uart} / 공중 {args.air})")

            if args.dry_run:
                print("--dry-run 이라 쓰지 않았다.")
                return 0

            # C0 = 저장하며 쓰기. 주소 0x02 에 1 바이트.
            ser.reset_input_buffer()
            ser.write(bytes([0xC0, 0x02, 0x01, reg0]))
            ser.flush()
            time.sleep(0.3)
            ser.read(32)

            after = read_registers(ser)
            if after is None:
                print("쓴 뒤 확인 읽기에 실패했다.", file=sys.stderr)
                return 1
            print("\n바꾼 뒤:")
            print(describe(after))

            if after[2] != reg0:
                print(f"\n경고: REG0 이 0x{reg0:02X} 로 안 들어갔다 "
                      f"(0x{after[2]:02X}). 전원을 껐다 켜고 다시 볼 것.",
                      file=sys.stderr)
                return 1

            print(f"\n완료. 이제 setting/port.yaml 에서 lora_baud 를 "
                  f"{args.uart} 로 맞출 것.")
            print("지상국 쪽 E220 도 **같은 값으로** 설정해야 한다. "
                  "한쪽만 바꾸면 링크가 끊긴다.")
    finally:
        # 무슨 일이 있어도 일반 전송 모드로 되돌린다. 설정 모드로 남으면
        # 텔레메트리가 통째로 멈춘다.
        try:
            set_mode(config=False)
        except Exception:
            print("경고: 일반 모드 복귀 실패. M0/M1 을 확인할 것.",
                  file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
