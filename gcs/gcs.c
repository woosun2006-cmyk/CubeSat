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
#include <sys/stat.h>
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

/* --- onboard log ---------------------------------------------------------
 *
 * UDP has no retransmission, and above a couple of hundred metres the LTE
 * link is expected to drop for seconds at a time. Whatever is lost then
 * exists nowhere, because the laptop was the only recorder. So the same line
 * that goes out on the wire is also written here, on the aircraft, where a
 * dead link cannot reach it. After recovery the two files merge by timestamp.
 *
 * This reverses the earlier "writes nothing to the SD card" policy in
 * gcs.service. The cost is 196 B per packet -- 0.7 MB/h at 1 Hz, 14 MB/h at
 * 20 Hz -- against losing the flight data that matters most.
 *
 * Logging is best effort: it never blocks or kills telemetry. Any failure
 * disables it for the rest of the run and the UDP stream carries on.
 */

static FILE *log_file = NULL;
static int log_off = 0;
static char log_path[512];

static void log_open(void)
{
    const char *dir = getenv("GCS_LOG_DIR");
    if (!dir || !dir[0]) {
        /* Relative to gcs.service's WorkingDirectory (the gcs project
           dir), so this lands in ~/cubesat/log/gcs. gcs.sh normally
           passes an absolute path so the cwd cannot matter. */
        dir = "../log/gcs";
    }
    if (!strcmp(dir, "none")) {
        log_off = 1;
        return;
    }
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        fprintf(stderr, "gcs: cannot create %s: %s\n", dir, strerror(errno));
        log_off = 1;
        return;
    }

    time_t now = time(NULL);
    struct tm parts;
    localtime_r(&now, &parts);
    char base[32];
    strftime(base, sizeof(base), "%m%d-%H%M", &parts);

    /* Restart=always means a crash loop can reopen inside the same minute;
       never clobber the previous run's file. Same naming as the laptop. */
    for (int n = 1; n < 100; ++n) {
        if (n == 1)
            snprintf(log_path, sizeof(log_path), "%s/%s-log.jsonl", dir, base);
        else
            snprintf(log_path, sizeof(log_path), "%s/%s-%d-log.jsonl",
                     dir, base, n);
        if (access(log_path, F_OK) != 0)
            break;
    }

    log_file = fopen(log_path, "wb");
    if (!log_file) {
        fprintf(stderr, "gcs: cannot open %s: %s\n", log_path, strerror(errno));
        log_off = 1;
        return;
    }
    fprintf(stdout, "gcs: onboard log %s\n", log_path);
    fflush(stdout);
}

static void log_write(const char *packet, size_t length)
{
    if (log_off)
        return;
    if (!log_file) {
        log_open();                   /* opened lazily: no empty files */
        if (!log_file)
            return;
    }
    if (fwrite(packet, 1, length, log_file) != length) {
        fprintf(stderr, "gcs: onboard log write failed: %s\n", strerror(errno));
        fclose(log_file);
        log_file = NULL;
        log_off = 1;
    }
}

/* Power on a sounding payload can vanish without warning, so the file has to
   be durable on disk, not just in libc's buffer. */
static void log_sync(void)
{
    if (log_file) {
        fflush(log_file);
        fsync(fileno(log_file));
    }
}

static void log_close(void)
{
    if (log_file) {
        log_sync();
        fclose(log_file);
        log_file = NULL;
    }
}


int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] :
        (getenv("PIXHAWK_DEVICE") ? getenv("PIXHAWK_DEVICE") : "/dev/ttyACM0");
    const char *destination_text = argc > 2 ? argv[2] :
        (getenv("GCS_DESTINATION") ? getenv("GCS_DESTINATION") : "10.0.0.2:14550");
    const int baud = argc > 3 ? atoi(argv[3]) :
        (getenv("PIXHAWK_BAUD") ? atoi(getenv("PIXHAWK_BAUD")) : 460800);

    /* How often a telemetry packet goes out. The link measures clean to
       50 Hz, the Pixhawk streams to 100, so this is a policy choice about
       resolution and mobile data, not a technical ceiling. */
    double send_hz = argc > 4 ? atof(argv[4]) :
        (getenv("GCS_SEND_HZ") ? atof(getenv("GCS_SEND_HZ")) : 1.0);
    if (!(send_hz > 0.0) || send_hz > 50.0) {
        fprintf(stderr, "gcs: send rate %g Hz out of range (0 < hz <= 50)\n",
                send_hz);
        return 2;
    }
    const uint64_t send_interval_ms = (uint64_t)(1000.0 / send_hz + 0.5);

    /* The send timer can only fire when poll returns, so the poll timeout
       has to be short compared with the interval or the rate comes out
       lumpy. 250 ms was fine at 1 Hz and is not at 10. */
    int poll_ms = (int)(send_interval_ms / 2);
    if (poll_ms > 250)
        poll_ms = 250;
    if (poll_ms < 5)
        poll_ms = 5;

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
    fprintf(stdout, "gcs: Pixhawk=%s baud=%d UDP=%s send=%g Hz\n",
            device, baud, destination_text, send_hz);
    fflush(stdout);

    uint64_t last_report = 0;
    uint64_t last_console_report = 0;
    uint64_t last_heartbeat = 0;
    uint64_t last_stream_request = 0;
    uint64_t last_sync = 0;
    while (keep_running) {
        if (reader.connection.fd < 0) {
            if (data_reader_open(&reader, device, baud) != 0) {
                fprintf(stderr, "gcs: waiting for %s: %s\n", device, strerror(errno));
                sleep(2);
                continue;
            }
            fprintf(stdout, "gcs: Pixhawk serial connected\n");
            fflush(stdout);
            /* Ask on the very next pass rather than waiting a cycle. */
            last_heartbeat = 0;
            last_stream_request = 0;
        }

        const int result = data_reader_poll(&reader, poll_ms);
        if (result < 0) {
            fprintf(stderr, "gcs: Pixhawk read error: %s; reconnecting\n", strerror(errno));
            data_reader_close(&reader);
            sleep(1);
            continue;
        }
        const uint64_t now = unix_time_ms();

        /* The Pixhawk streams only what a GCS asks for. Announce
         * ourselves every second and renew the stream request every
         * five, so an autopilot reboot or a re-plugged USB recovers
         * without anyone touching the aircraft. */
        if (now - last_heartbeat >= 1000) {
            const int ask_streams = (now - last_stream_request >= 5000);
            if (data_reader_announce(&reader, ask_streams, send_hz) != 0) {
                fprintf(stderr, "gcs: Pixhawk write failed: %s\n",
                        strerror(errno));
            } else if (ask_streams) {
                last_stream_request = now;
            }
            last_heartbeat = now;
        }

        if (now - last_report >= send_interval_ms) {
            char packet[768];
            const size_t packet_size = make_packet(packet, sizeof(packet),
                                                   data_reader_telemetry(&reader));
            if (packet_size > 0) {
                /* Record before transmitting: a failed send must not
                   cost us the sample. */
                log_write(packet, packet_size);
                if (sendto(udp_fd, packet, packet_size, 0,
                           (const struct sockaddr *)&destination,
                           sizeof(destination)) < 0) {
                    fprintf(stderr, "gcs: UDP send failed: %s\n",
                            strerror(errno));
                }
            }
            last_report = now;
            if (now - last_sync >= 2000) {
                log_sync();
                last_sync = now;
            }
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
    log_close();
    fprintf(stdout, "gcs: stopped\n");
    return 0;
}
