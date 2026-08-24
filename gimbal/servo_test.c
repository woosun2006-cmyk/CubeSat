#include <stdio.h>
#include <wiringPi.h>
#include <softPwm.h>

#define SERVO 1

int servoControl() {
	softPwmCreate(SERVO, 0, 200);
	softPwmWrite(SERVO, 5);
	delay(600);
	softPwmWrite(SERVO, 25);
	delay(600);

	return 0;
}

int main() {
	if(wiringPiSetup() == -1) {
		return -1;
	}

	servoControl();
	return 0;
}
