#include <stdio.h>
#include <unistd.h>
#include <wiringPi.h>
#include <softPwm.h>
#include <mavlink/common/mavlink.h>
#include "mav_serial.h"

#define SERIAL_PORT "/dev/ttyACM0"

#define PITCH_PIN 1   // wiringPi 1 = GPIO18, physical pin 12
#define ROLL_PIN  26  // wiringPi 26 = GPIO12, physical pin 32

#define PWM_RANGE 200
#define DUTY_CENTER 15
#define DUTY_HALF_RANGE 5   // duty 10~20 (1.0ms~2.0ms)
#define RAD2DEG (180.0 / 3.14159265358979323846)

static int angleToDuty(double angle_deg) {
    int duty = DUTY_CENTER - (int)(angle_deg * (DUTY_HALF_RANGE / 90.0));
    if (duty < DUTY_CENTER - DUTY_HALF_RANGE) duty = DUTY_CENTER - DUTY_HALF_RANGE;
    if (duty > DUTY_CENTER + DUTY_HALF_RANGE) duty = DUTY_CENTER + DUTY_HALF_RANGE;
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
    if (softPwmCreate(PITCH_PIN, 0, PWM_RANGE) != 0 ||
        softPwmCreate(ROLL_PIN, 0, PWM_RANGE) != 0) {
        printf("softPwmCreate failed\n");
        return 1;
    }
    softPwmWrite(PITCH_PIN, DUTY_CENTER);
    softPwmWrite(ROLL_PIN, DUTY_CENTER);

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
        double pitch_deg = att.pitch * RAD2DEG;
        double roll_deg = att.roll * RAD2DEG;

        int pitch_duty = angleToDuty(pitch_deg);
        int roll_duty = angleToDuty(roll_deg);
        softPwmWrite(PITCH_PIN, pitch_duty);
        softPwmWrite(ROLL_PIN, roll_duty);

        printf("pitch=%.1fdeg(duty=%d) roll=%.1fdeg(duty=%d)\n",
               pitch_deg, pitch_duty, roll_deg, roll_duty);
    }

    return 0;
}
