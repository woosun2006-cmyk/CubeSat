#define _DEFAULT_SOURCE
#include "MAVLink.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define MAVLINK_STX_V1 0xfeu
#define MAVLINK_STX_V2 0xfdu
#define MAVLINK_MAX_PAYLOAD 255u

static speed_t baud_to_termios(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

static uint16_t crc_accumulate(uint8_t byte, uint16_t crc)
{
    uint8_t tmp = byte ^ (uint8_t)(crc & 0xffu);
    tmp ^= (uint8_t)(tmp << 4);
    return (uint16_t)((crc >> 8) ^ ((uint16_t)tmp << 8) ^
                      ((uint16_t)tmp << 3) ^ ((uint16_t)tmp >> 4));
}

static uint16_t mavlink_crc(const uint8_t *frame, size_t header_len,
                            uint8_t crc_extra)
{
    uint16_t crc = 0xffffu;
    /* The magic byte is not included in the MAVLink checksum. */
    for (size_t i = 1; i < header_len; ++i)
        crc = crc_accumulate(frame[i], crc);
    for (size_t i = header_len; i < header_len + frame[1]; ++i)
        crc = crc_accumulate(frame[i], crc);
    crc = crc_accumulate(crc_extra, crc);
    return crc;
}

static uint8_t crc_extra_for(uint32_t msgid)
{
    switch (msgid) {
    case 0: return 50;   /* HEARTBEAT */
    case 24: return 24;  /* GPS_RAW_INT */
    case 30: return 39;  /* ATTITUDE */
    case 33: return 104; /* GLOBAL_POSITION_INT */
    default: return 0;
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t get_i16(const uint8_t *p)
{
    return (int16_t)get_u16(p);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t get_i32(const uint8_t *p)
{
    return (int32_t)get_u32(p);
}

static float get_float(const uint8_t *p)
{
    float value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint32_t frame_msgid(const uint8_t *frame)
{
    if (frame[0] == MAVLINK_STX_V1)
        return frame[5];
    return (uint32_t)frame[7] | ((uint32_t)frame[8] << 8) |
           ((uint32_t)frame[9] << 16);
}

static void decode_frame(const uint8_t *frame, size_t header_len,
                         MavlinkTelemetry *telemetry)
{
    const uint8_t payload_len = frame[1];
    const uint8_t *payload = frame + header_len;
    const uint32_t msgid = frame_msgid(frame);

    if (msgid == 30 && payload_len >= 16) {
        telemetry->attitude_time_boot_ms = get_u32(payload);
        telemetry->roll = get_float(payload + 4);
        telemetry->pitch = get_float(payload + 8);
        telemetry->yaw = get_float(payload + 12);
        telemetry->attitude_valid = 1;
    } else if (msgid == 24 && payload_len >= 30) {
        telemetry->gps_fix_type = payload[8];
        telemetry->latitude_e7 = get_i32(payload + 9);
        telemetry->longitude_e7 = get_i32(payload + 13);
        telemetry->altitude_mm = get_i32(payload + 17);
        telemetry->ground_course_cdeg = get_u16(payload + 27);
        telemetry->gps_satellites = payload[29];
        telemetry->gps_valid = (telemetry->gps_fix_type >= 2 &&
                                telemetry->latitude_e7 != 0 &&
                                telemetry->longitude_e7 != 0);
    } else if (msgid == 33 && payload_len >= 28) {
        telemetry->latitude_e7 = get_i32(payload + 4);
        telemetry->longitude_e7 = get_i32(payload + 8);
        telemetry->altitude_mm = get_i32(payload + 12);
        telemetry->relative_altitude_mm = get_i32(payload + 16);
        telemetry->velocity_x_cms = get_i16(payload + 20);
        telemetry->velocity_y_cms = get_i16(payload + 22);
        telemetry->velocity_z_cms = get_i16(payload + 24);
        telemetry->ground_course_cdeg = get_u16(payload + 26);
        telemetry->gps_valid = (telemetry->latitude_e7 != 0 &&
                                telemetry->longitude_e7 != 0);
    }
}

int mavlink_open(MavlinkConnection *connection, const char *device, int baud)
{
    if (!connection || !device || baud_to_termios(baud) == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(connection, 0, sizeof(*connection));
    connection->fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (connection->fd < 0)
        return -1;

    struct termios tty;
    if (tcgetattr(connection->fd, &tty) != 0) {
        close(connection->fd);
        connection->fd = -1;
        return -1;
    }
    cfmakeraw(&tty);
    cfsetispeed(&tty, baud_to_termios(baud));
    cfsetospeed(&tty, baud_to_termios(baud));
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(connection->fd, TCSANOW, &tty) != 0) {
        close(connection->fd);
        connection->fd = -1;
        return -1;
    }
    return 0;
}

void mavlink_close(MavlinkConnection *connection)
{
    if (connection && connection->fd >= 0) {
        close(connection->fd);
        connection->fd = -1;
    }
}

static int parser_push(MavlinkConnection *connection, uint8_t byte,
                       MavlinkTelemetry *telemetry)
{
    if (connection->frame_len == 0) {
        if (byte != MAVLINK_STX_V1 && byte != MAVLINK_STX_V2)
            return 0;
        connection->header_len = (byte == MAVLINK_STX_V1) ? 6 : 10;
    }
    if (connection->frame_len >= sizeof(connection->frame)) {
        connection->frame_len = 0;
        connection->expected_len = 0;
        return 0;
    }
    connection->frame[connection->frame_len++] = byte;
    if (connection->frame_len == 2) {
        connection->expected_len = connection->header_len + connection->frame[1] + 2;
    }
    if (connection->expected_len == 0 ||
        connection->frame_len < connection->expected_len)
        return 0;

    const size_t crc_offset = connection->expected_len - 2;
    const uint32_t msgid = frame_msgid(connection->frame);
    const uint16_t received_crc = get_u16(connection->frame + crc_offset);
    const uint16_t calculated_crc = mavlink_crc(connection->frame,
                                                connection->header_len,
                                                crc_extra_for(msgid));
    connection->frame_len = 0;
    connection->expected_len = 0;
    if (received_crc != calculated_crc) {
        telemetry->packets_bad_crc++;
        return -1;
    }
    decode_frame(connection->frame, connection->header_len, telemetry);
    telemetry->packets_received++;
    return 1;
}

int mavlink_poll(MavlinkConnection *connection, MavlinkTelemetry *telemetry,
                 int timeout_ms)
{
    if (!connection || !telemetry || connection->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    struct pollfd descriptor = { connection->fd, POLLIN, 0 };
    int ready;
    do {
        ready = poll(&descriptor, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0)
        return ready;
    uint8_t bytes[256];
    const ssize_t count = read(connection->fd, bytes, sizeof(bytes));
    if (count < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        return -1;
    }
    int frames = 0;
    for (ssize_t i = 0; i < count; ++i) {
        const int result = parser_push(connection, bytes[i], telemetry);
        if (result > 0) {
            frames += result;
            telemetry->last_rx_time_us = 0; /* filled by data.c's clock */
        }
    }
    return frames;
}
