#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <wiringPi.h>
#include <softPwm.h>
#include "zero_point.h"

// WASD 로 서보 영점(기준점)을 맞추는 대화형 프로그램.
//   w/s : pitch 조정   a/d : roll 조정
//   space : zero_point.yaml 에 저장   q : 종료

#define PITCH_PIN 1   // wiringPi 1 = GPIO18, physical pin 12
#define ROLL_PIN  26  // wiringPi 26 = GPIO12, physical pin 32
#define PWM_RANGE 200
#define DUTY_MIN 10   // 1.0ms
#define DUTY_MAX 20   // 2.0ms

static struct termios orig_termios;

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void set_raw_terminal(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(unsigned int)(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void handle_sigint(int sig) {
    (void)sig;
    restore_terminal();
    printf("\n");
    exit(0);
}

static int clamp(int v) {
    if (v < DUTY_MIN) return DUTY_MIN;
    if (v > DUTY_MAX) return DUTY_MAX;
    return v;
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

    int pitch, roll;
    load_zero_point(&pitch, &roll);
    softPwmWrite(PITCH_PIN, pitch);
    softPwmWrite(ROLL_PIN, roll);

    printf("서보 영점(기준점) 맞추기 - WASD 조작\n");
    printf("  w/s : pitch 조정   a/d : roll 조정\n");
    printf("  space : 현재 값을 zero_point.yaml 에 저장   q : 저장 없이 종료\n\n");

    set_raw_terminal();
    atexit(restore_terminal);
    signal(SIGINT, handle_sigint);

    int dirty = 0;
    while (1) {
        printf("\r[pitch=%2d roll=%2d]%s   ", pitch, roll, dirty ? " *저장 안 됨" : "            ");
        fflush(stdout);

        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        if (c == 'q') {
            printf("\n");
            if (dirty) printf("저장하지 않은 변경사항이 있습니다.\n");
            break;
        }
        if (c == ' ') {
            if (save_zero_point(pitch, roll) == 0) {
                printf("\n저장됨 -> zero_point.yaml (pitch=%d roll=%d)\n", pitch, roll);
                dirty = 0;
            } else {
                printf("\n저장 실패\n");
            }
            continue;
        }

        int moved = 1;
        switch (c) {
            case 'w': pitch = clamp(pitch - 1); break;
            case 's': pitch = clamp(pitch + 1); break;
            case 'a': roll = clamp(roll - 1); break;
            case 'd': roll = clamp(roll + 1); break;
            default: moved = 0; break;
        }
        if (moved) {
            dirty = 1;
            softPwmWrite(PITCH_PIN, pitch);
            softPwmWrite(ROLL_PIN, roll);
        }
    }

    return 0;
}
