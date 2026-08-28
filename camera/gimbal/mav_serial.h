#ifndef MAV_SERIAL_H
#define MAV_SERIAL_H

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>

static int mav_serial_open(const char *port) {
    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    struct termios tty;
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tty);

    return fd;
}

#endif
