#define _DEFAULT_SOURCE
#include "data.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_time_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ull + (uint64_t)now.tv_nsec / 1000ull;
}

static void print_telemetry(const MavlinkTelemetry *telemetry)
{
    if (telemetry->attitude_valid) {
        printf("ATTITUDE roll=%.6f pitch=%.6f yaw=%.6f\n",
               telemetry->roll, telemetry->pitch, telemetry->yaw);
    } else {
        printf("ATTITUDE status=NO_DATA\n");
    }

    if (telemetry->gps_valid) {
        printf("GPS fix=%u sats=%u lat=%.7f lon=%.7f alt_m=%.3f\n",
               telemetry->gps_fix_type, telemetry->gps_satellites,
               telemetry->latitude_e7 / 1e7, telemetry->longitude_e7 / 1e7,
               telemetry->altitude_mm / 1000.0);
    } else {
        printf("GPS status=NO_FIX fix=%u sats=%u\n",
               telemetry->gps_fix_type, telemetry->gps_satellites);
    }
    printf("MAVLINK packets=%u bad_crc=%u\n",
           telemetry->packets_received, telemetry->packets_bad_crc);
    fflush(stdout);
}

int data_reader_open(DataReader *reader, const char *device, int baud)
{
    if (!reader) {
        errno = EINVAL;
        return -1;
    }
    memset(reader, 0, sizeof(*reader));
    reader->connection.fd = -1;
    return mavlink_open(&reader->connection, device, baud);
}

int data_reader_poll(DataReader *reader, int timeout_ms)
{
    if (!reader) {
        errno = EINVAL;
        return -1;
    }
    const int result = mavlink_poll(&reader->connection, &reader->telemetry,
                                    timeout_ms);
    if (result > 0)
        reader->telemetry.last_rx_time_us = monotonic_time_us();
    return result;
}

const MavlinkTelemetry *data_reader_telemetry(const DataReader *reader)
{
    return reader ? &reader->telemetry : NULL;
}

int data_reader_announce(DataReader *reader, int request_streams,
                         double send_hz)
{
    if (!reader || reader->connection.fd < 0) {
        errno = EINVAL;
        return -1;
    }
    int result = mavlink_send_heartbeat(&reader->connection);
    if (request_streams &&
        mavlink_request_streams(&reader->connection,
                                &reader->telemetry, send_hz) != 0)
        result = -1;
    return result;
}

void data_reader_close(DataReader *reader)
{
    if (reader)
        mavlink_close(&reader->connection);
}

int data_read_loop(const char *device, int baud)
{
    DataReader reader;

    if (data_reader_open(&reader, device, baud) != 0) {
        fprintf(stderr, "data: cannot open Pixhawk serial %s at %d baud: %s\n",
                device, baud, strerror(errno));
        return 1;
    }
    printf("data: listening to Pixhawk on %s at %d baud\n", device, baud);
    printf("data: GPS may be unavailable; this is reported as GPS status=NO_FIX\n");

    uint64_t last_report = 0;
    for (;;) {
        const int result = data_reader_poll(&reader, 1000);
        if (result < 0) {
            fprintf(stderr, "data: serial read failed: %s\n", strerror(errno));
            data_reader_close(&reader);
            return 1;
        }
        const uint64_t now = monotonic_time_us();
        if (result > 0 || now - last_report >= 1000000ull) {
            last_report = now;
            print_telemetry(data_reader_telemetry(&reader));
        }
    }
}

#ifdef DATA_STANDALONE
int main(int argc, char **argv)
{
    const char *device = (argc > 1) ? argv[1] : "/dev/serial0";
    const int baud = (argc > 2) ? atoi(argv[2]) : 460800;
    return data_read_loop(device, baud);
}
#endif
