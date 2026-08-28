#include <stdio.h>
#include <wiringPi.h>
#include <softPwm.h>

#define SERVO 1

int main(void)
{
    if (wiringPiSetup() == -1) {
        printf("wiringPi setup failed\n");
        return 1;
    }

    // softPwm 범위: 0 ~ 200
    if (softPwmCreate(SERVO, 0, 200) != 0) {
        printf("softPwmCreate failed\n");
        return 1;
    }

    printf("Servo test start\n");

    // 가운데
    softPwmWrite(SERVO, 15);
    delay(1000);

    // 한쪽
    softPwmWrite(SERVO, 10);
    delay(1000);

    // 반대쪽
    softPwmWrite(SERVO, 20);
    delay(1000);

    // 가운데
    softPwmWrite(SERVO, 15);
    delay(1000);

    return 0;
}
