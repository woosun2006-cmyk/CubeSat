#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wiringPi.h>
#include <softPwm.h>
#include <mavlink/common/mavlink.h>
#include "mav_serial.h"
#include "zero_point.h"

// Pixhawk 는 MAVLink 포트를 둘 노출한다. gcs 가 ttyACM0 을 쓰는 동안
// 짐벌을 함께 돌리려면 여기를 ttyACM1 로 잡아야 한다. 같은 장치를 두
// 프로세스가 열면 바이트를 나눠 가져 양쪽 파싱이 모두 깨진다.
//   gimbal_full                 기본 ttyACM0
//   gimbal_full /dev/ttyACM1    인자로 지정
//   GIMBAL_SERIAL=... gimbal_full
#define SERIAL_PORT_DEFAULT "/dev/ttyACM0"

#define PITCH_PIN 1   // wiringPi 1 = GPIO18, physical pin 12
#define ROLL_PIN  26  // wiringPi 26 = GPIO12, physical pin 32

#define PWM_RANGE 200
#define DUTY_HALF_RANGE 10  // duty center±10, 최대 5~25 (0.5ms~2.5ms) 까지 확장 가능
#define DUTY_MIN 5   // 0.5ms — servo_test.c 에서 왕복 검증된 최소값
#define DUTY_MAX 25  // 2.5ms — servo_test.c 에서 왕복 검증된 최대값
#define RAD2DEG (180.0 / 3.14159265358979323846)

// zero_point.yaml 로 맞춘 영점(center)을 기준으로 좌우/앞뒤 duty 를 계산한다.
// DUTY_MIN/MAX 로 절대 클램프해서 center 값에 관계없이 검증된 범위를 벗어나지 않는다.
static int angleToDuty(double angle_deg, int center) {
    int duty = center - (int)(angle_deg * (DUTY_HALF_RANGE / 90.0));
    if (duty < center - DUTY_HALF_RANGE) duty = center - DUTY_HALF_RANGE;
    if (duty > center + DUTY_HALF_RANGE) duty = center + DUTY_HALF_RANGE;
    if (duty < DUTY_MIN) duty = DUTY_MIN;
    if (duty > DUTY_MAX) duty = DUTY_MAX;
    return duty;
}

static void request_attitude(int fd, uint8_t sysid, uint8_t compid) {
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    mavlink_msg_command_long_pack(255, 190, &msg, sysid, compid,
        MAV_CMD_SET_MESSAGE_INTERVAL, 0, MAVLINK_MSG_ID_ATTITUDE, 50000, 0, 0, 0, 0, 0);
    int len = mavlink_msg_to_send_buffer(buf, &msg);
    write(fd, buf, len);
}

int main(int argc, char **argv) {
    const char *serial_port = (argc > 1) ? argv[1] :
        (getenv("GIMBAL_SERIAL") ? getenv("GIMBAL_SERIAL") : SERIAL_PORT_DEFAULT);

    if (wiringPiSetup() == -1) {
        printf("wiringPi setup failed\n");
        return 1;
    }
    if (softPwmCreate(PITCH_PIN, 0, PWM_RANGE) != 0 ||
        softPwmCreate(ROLL_PIN, 0, PWM_RANGE) != 0) {
        printf("softPwmCreate failed\n");
        return 1;
    }

    int pitch_center, roll_center;
    load_zero_point(&pitch_center, &roll_center);
    softPwmWrite(PITCH_PIN, pitch_center);
    softPwmWrite(ROLL_PIN, roll_center);

    printf("gimbal: MAVLink %s\n", serial_port);
    int fd = mav_serial_open(serial_port);
    if (fd < 0) return 1;

    mavlink_message_t msg;
    mavlink_status_t status;
    uint8_t b;
    uint8_t sysid = 0, compid = 0;

    printf("waiting for heartbeat...\n");
    while (sysid == 0) {
        if (read(fd, &b, 1) != 1) continue;
        if (mavlink_parse_char(MAVLINK_COMM_0, b, &msg, &status) &&
            msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
            sysid = msg.sysid;
            compid = msg.compid;
        }
    }
    printf("heartbeat ok: sysid=%d compid=%d\n", sysid, compid);

    request_attitude(fd, sysid, compid);

    while (1) {
        if (read(fd, &b, 1) != 1) continue;
        if (!mavlink_parse_char(MAVLINK_COMM_0, b, &msg, &status)) continue;
        if (msg.msgid != MAVLINK_MSG_ID_ATTITUDE) continue;

        mavlink_attitude_t att;
        mavlink_msg_attitude_decode(&msg, &att);
        double pitch_deg = att.pitch * RAD2DEG;
        double roll_deg = att.roll * RAD2DEG;

        int pitch_duty = angleToDuty(pitch_deg, pitch_center);
        int roll_duty = angleToDuty(roll_deg, roll_center);
        softPwmWrite(PITCH_PIN, pitch_duty);
        softPwmWrite(ROLL_PIN, roll_duty);

        printf("pitch=%.1fdeg(duty=%d) roll=%.1fdeg(duty=%d)\n",
               pitch_deg, pitch_duty, roll_deg, roll_duty);
    }

    return 0;
}
