#include <stdio.h>
#include <unistd.h>
#include <wiringPi.h>
#include <softPwm.h>
#include <mavlink/common/mavlink.h>
#include "mav_serial.h"
#include "zero_point.h"

#define SERIAL_PORT "/dev/ttyACM0"

#define SERVO_PIN 26  // wiringPi 26 = GPIO12, physical pin 32 (was servo2_test.c)
#define PWM_RANGE 200
#define DUTY_HALF_RANGE 10  // duty center±10, 최대 5~25 (0.5ms~2.5ms) 까지 확장 가능
#define DUTY_MIN 5   // 0.5ms — servo_test.c 에서 왕복 검증된 최소값
#define DUTY_MAX 25  // 2.5ms — servo_test.c 에서 왕복 검증된 최대값
#define RAD2DEG (180.0 / 3.14159265358979323846)

// zero_point.yaml 로 맞춘 영점(center)을 기준으로 duty 를 계산한다.
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

int main(void) {
    if (wiringPiSetup() == -1) {
        printf("wiringPi setup failed\n");
        return 1;
    }
    if (softPwmCreate(SERVO_PIN, 0, PWM_RANGE) != 0) {
        printf("softPwmCreate failed\n");
        return 1;
    }

    int pitch_center, roll_center;
    load_zero_point(&pitch_center, &roll_center);
    (void)pitch_center;
    softPwmWrite(SERVO_PIN, roll_center);

    int fd = mav_serial_open(SERIAL_PORT);
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
        double roll_deg = att.roll * RAD2DEG;

        int duty = angleToDuty(roll_deg, roll_center);
        softPwmWrite(SERVO_PIN, duty);
        printf("roll=%.1fdeg -> duty=%d\n", roll_deg, duty);
    }

    return 0;
}
