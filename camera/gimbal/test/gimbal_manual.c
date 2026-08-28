#include <stdio.h>
#include <wiringPi.h>
#include <softPwm.h>

#define SERVO_TILT 1   // GPIO18, physical pin 12, forward-backward
#define SERVO_PAN  26  // GPIO12, physical pin 32, left-right

#define PWM_RANGE 200
#define DUTY_MIN 10    // 1.0ms
#define DUTY_MAX 20    // 2.0ms

static int angleToDuty(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    return DUTY_MIN + (angle * (DUTY_MAX - DUTY_MIN)) / 180;
}

static void setTilt(int angle) {
    softPwmWrite(SERVO_TILT, angleToDuty(angle));
}

static void setPan(int angle) {
    softPwmWrite(SERVO_PAN, angleToDuty(angle));
}

int main(void)
{
    if (wiringPiSetup() == -1) {
        printf("wiringPi setup failed\n");
        return 1;
    }

    if (softPwmCreate(SERVO_TILT, 0, PWM_RANGE) != 0 ||
        softPwmCreate(SERVO_PAN, 0, PWM_RANGE) != 0) {
        printf("softPwmCreate failed\n");
        return 1;
    }

    printf("Gimbal control. Enter: <pan 0-180> <tilt 0-180>, or 'q' to quit.\n");

    setPan(90);
    setTilt(90);
    delay(500);

    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'q') break;

        int pan, tilt;
        if (sscanf(line, "%d %d", &pan, &tilt) == 2) {
            setPan(pan);
            setTilt(tilt);
            printf("pan=%d tilt=%d\n", pan, tilt);
        } else {
            printf("format: <pan> <tilt>\n");
        }
    }

    return 0;
}
