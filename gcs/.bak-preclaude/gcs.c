#define _DEFAULT_SOURCE
#include "data.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void stop_handler(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static uint64_t unix_time_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (uint64_t)now.tv_sec * 1000ull + (uint64_t)now.tv_nsec / 1000000ull;
}

static int parse_port(const char *text, unsigned short *port)
{
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (!text[0] || !end || *end || value < 1 || value > 65535)
        return -1;
    *port = (unsigned short)value;
    return 0;
}

static int parse_destination(const char *text, struct sockaddr_in *destination)
{
    char endpoint[128];
    const char *separator = strrchr(text, ':');
    if (!separator || separator == text || separator[1] == '\0' ||
        (size_t)(separator - text) >= sizeof(endpoint))
        return -1;
    memset(destination, 0, sizeof(*destination));
    memcpy(endpoint, text, (size_t)(separator - text));
    endpoint[separator - text] = '\0';
    unsigned short port;
    if (parse_port(separator + 1, &port) != 0 ||
        inet_pton(AF_INET, endpoint, &destination->sin_addr) != 1)
        return -1;
    destination->sin_family = AF_INET;
    destination->sin_port = htons(port);
    return 0;
}

static size_t make_packet(char *packet, size_t packet_size,
                          const MavlinkTelemetry *telemetry)
{
    const double latitude = telemetry->latitude_e7 / 1e7;
    const double longitude = telemetry->longitude_e7 / 1e7;
    const double altitude = telemetry->altitude_mm / 1000.0;
    const int written = snprintf(
        packet, packet_size,
        "{\"ts_ms\":%llu,\"attitude_valid\":%u,\"roll\":%.7g,\"pitch\":%.7g,\"yaw\":%.7g,"
        "\"gps_valid\":%u,\"gps_fix\":%u,\"gps_sats\":%u,\"lat\":%.10g,\"lon\":%.10g,"
        "\"alt_m\":%.7g,\"rel_alt_m\":%.7g,\"vx_cms\":%d,\"vy_cms\":%d,\"vz_cms\":%d,"
        "\"hdg_cdeg\":%u}\n",
        (unsigned long long)unix_time_ms(), telemetry->attitude_valid,
        telemetry->roll, telemetry->pitch, telemetry->yaw,
        telemetry->gps_valid, telemetry->gps_fix_type, telemetry->gps_satellites,
        latitude, longitude, altitude, telemetry->relative_altitude_mm / 1000.0,
        telemetry->velocity_x_cms, telemetry->velocity_y_cms,
        telemetry->velocity_z_cms, telemetry->ground_course_cdeg);
    if (written < 0 || (size_t)written >= packet_size)
        return 0;
    return (size_t)written;
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] :
        (getenv("PIXHAWK_DEVICE") ? getenv("PIXHAWK_DEVICE") : "/dev/ttyACM0");
    const char *destination_text = argc > 2 ? argv[2] :
        (getenv("GCS_DESTINATION") ? getenv("GCS_DESTINATION") : "10.0.0.2:14550");
    const int baud = argc > 3 ? atoi(argv[3]) :
        (getenv("PIXHAWK_BAUD") ? atoi(getenv("PIXHAWK_BAUD")) : 460800);

    struct sockaddr_in destination;
    if (parse_destination(destination_text, &destination) != 0) {
        fprintf(stderr, "gcs: invalid destination '%s' (use IPv4:PORT)\n", destination_text);
        return 2;
    }
    const int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("gcs: UDP socket");
        return 1;
    }
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    DataReader reader;
    memset(&reader, 0, sizeof(reader));
    reader.connection.fd = -1;
    fprintf(stdout, "gcs: Pixhawk=%s baud=%d UDP=%s\n",
            device, baud, destination_text);
    fflush(stdout);

    uint64_t last_report = 0;
    uint64_t last_console_report = 0;
    while (keep_running) {
        if (reader.connection.fd < 0) {
            if (data_reader_open(&reader, device, baud) != 0) {
                fprintf(stderr, "gcs: waiting for %s: %s\n", device, strerror(errno));
                sleep(2);
                continue;
            }
            fprintf(stdout, "gcs: Pixhawk serial connected\n");
            fflush(stdout);
        }

        const int result = data_reader_poll(&reader, 250);
        if (result < 0) {
            fprintf(stderr, "gcs: Pixhawk read error: %s; reconnecting\n", strerror(errno));
            data_reader_close(&reader);
            sleep(1);
            continue;
        }
        const uint64_t now = unix_time_ms();
        if (now - last_report >= 1000) {
            char packet[768];
            const size_t packet_size = make_packet(packet, sizeof(packet),
                                                   data_reader_telemetry(&reader));
            if (packet_size > 0 && sendto(udp_fd, packet, packet_size, 0,
                                          (const struct sockaddr *)&destination,
                                          sizeof(destination)) < 0) {
                fprintf(stderr, "gcs: UDP send failed: %s\n", strerror(errno));
            }
            last_report = now;
            if (now - last_console_report >= 1000) {
                const MavlinkTelemetry *snapshot =
                    data_reader_telemetry(&reader);
                if (snapshot->attitude_valid) {
                    fprintf(stdout,
                            "gcs: IMU roll=%+.4f pitch=%+.4f yaw=%+.4f ",
                            snapshot->roll, snapshot->pitch, snapshot->yaw);
                } else {
                    fprintf(stdout, "gcs: IMU=NO_DATA ");
                }
                if (snapshot->gps_valid) {
                    fprintf(stdout, "GPS fix=%u sats=%u lat=%.7f lon=%.7f alt=%.2fm ",
                            snapshot->gps_fix_type, snapshot->gps_satellites,
                            snapshot->latitude_e7 / 1e7,
                            snapshot->longitude_e7 / 1e7,
                            snapshot->altitude_mm / 1000.0);
                } else {
                    fprintf(stdout, "GPS=NO_FIX ");
                }
                fputc('\n', stdout);
                fflush(stdout);
                last_console_report = now;
            }
        }
    }

    data_reader_close(&reader);
    close(udp_fd);
    fprintf(stdout, "gcs: stopped\n");
    return 0;
}
