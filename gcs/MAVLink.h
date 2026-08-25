#ifndef CUBESAT_MAVLINK_H
#define CUBESAT_MAVLINK_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int fd;
    uint8_t frame[300];
    size_t frame_len;
    size_t expected_len;
    uint8_t header_len;
} MavlinkConnection;

typedef struct {
    uint8_t attitude_valid;
    uint32_t attitude_time_boot_ms;
    float roll;
    float pitch;
    float yaw;

    uint8_t gps_valid;
    uint8_t gps_fix_type;
    uint8_t gps_satellites;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_mm;
    int32_t relative_altitude_mm;
    int16_t velocity_x_cms;
    int16_t velocity_y_cms;
    int16_t velocity_z_cms;
    uint16_t ground_course_cdeg;

    uint64_t last_rx_time_us;
    uint32_t packets_received;
    uint32_t packets_bad_crc;
} MavlinkTelemetry;

int mavlink_open(MavlinkConnection *connection, const char *device, int baud);
void mavlink_close(MavlinkConnection *connection);
int mavlink_poll(MavlinkConnection *connection, MavlinkTelemetry *telemetry,
                 int timeout_ms);

#endif
