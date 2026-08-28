#include <stdio.h>
#include <wiringPi.h>
#include <softPwm.h>

#define SERVO2 26  // GPIO12 / physical pin 32, left-right

int main(void)
{
    if (wiringPiSetup() == -1) {
        printf("wiringPi setup failed\n");
        return 1;
    }

    if (softPwmCreate(SERVO2, 0, 200) != 0) {
        printf("softPwmCreate failed\n");
        return 1;
    }

    printf("Servo2 test start\n");

    softPwmWrite(SERVO2, 15);
    delay(1000);

    softPwmWrite(SERVO2, 10);
    delay(1000);

    softPwmWrite(SERVO2, 20);
    delay(1000);

    softPwmWrite(SERVO2, 15);
    delay(1000);

    return 0;
}
