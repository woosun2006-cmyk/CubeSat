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
#include <fcntl.h>
#include <termios.h>

static volatile sig_atomic_t keep_running = 1;
static unsigned long long telemetry_seq = 0;

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

/* Whether the same packet also goes out over LoRa is decided at run time.
 * With nothing set the process runs exactly as before, LTE only -- that is
 * what gcs.service does. Only the caller that owns the LoRa link (lora.sh)
 * passes GCS_LORA_DEVICE=/dev/ttyAMA3, because mav.py writes to the same
 * UART and two writers on one UART interleave their lines. Handing the
 * device in from outside keeps that arbitration in one place. */
static const char *lora_device(void)
{
    const char *device = getenv("GCS_LORA_DEVICE");
    return (device && device[0]) ? device : NULL;
}

static speed_t lora_speed(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return (speed_t)0;
    }
}

static int open_lora(const char *device, int baud)
{
    const speed_t speed = lora_speed(baud);

    if (speed == (speed_t)0) {
        fprintf(
            stderr,
            "gcs: LoRa baud %d not supported\n",
            baud
        );
        return -1;
    }

    const int fd = open(
        device,
        O_RDWR | O_NOCTTY
    );

    if (fd < 0) {
        fprintf(
            stderr,
            "gcs: cannot open LoRa %s: %s\n",
            device,
            strerror(errno)
        );
        return -1;
    }

    struct termios tty;

    memset(&tty, 0, sizeof(tty));

    if (tcgetattr(fd, &tty) != 0) {
        fprintf(
            stderr,
            "gcs: LoRa tcgetattr failed: %s\n",
            strerror(errno)
        );
        close(fd);
        return -1;
    }

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag &= ~PARENB;          /* parity none */
    tty.c_cflag &= ~CSTOPB;          /* 1 stop bit */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;              /* 8 data bits */

    tty.c_cflag &= ~CRTSCTS;         /* flow control off */
    tty.c_cflag |= CREAD | CLOCAL;

    tty.c_lflag = 0;
    tty.c_iflag = 0;
    tty.c_oflag = 0;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(
            stderr,
            "gcs: LoRa tcsetattr failed: %s\n",
            strerror(errno)
        );
        close(fd);
        return -1;
    }

    fprintf(
        stdout,
        "gcs: LoRa connected %s @ %d\n",
        device,
        baud
    );
    fflush(stdout);

    return fd;
}


static int lora_write_all(
    int fd,
    const char *data,
    size_t length
)
{
    size_t sent = 0;

    while (sent < length) {

        const ssize_t n = write(
            fd,
            data + sent,
            length - sent
        );

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

/*
 * Explicit little-endian serializers.
 * The radio wire format is written byte-by-byte so compiler padding and
 * host endianness cannot change the packet layout.
 */
static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_i16_le(uint8_t *p, int16_t v)
{
    put_u16_le(p, (uint16_t)v);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_i32_le(uint8_t *p, int32_t v)
{
    put_u32_le(p, (uint32_t)v);
}

static void put_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}


/*
 * CRC-16/CCITT-FALSE
 * polynomial: 0x1021
 * initial value: 0xFFFF
 * no reflection
 */
static uint16_t crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;

        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            else
                crc <<= 1;
        }
    }

    return crc;
}

static size_t make_packet(
    char *packet,
    size_t packet_size,
    const MavlinkTelemetry *telemetry,
    unsigned long long seq,
    uint64_t ts_ms)
{
    const double latitude = telemetry->latitude_e7 / 1e7;
    const double longitude = telemetry->longitude_e7 / 1e7;
    const double altitude = telemetry->altitude_mm / 1000.0;
    const int written = snprintf(
        packet, packet_size,
        "{\"seq\":%llu,\"ts_ms\":%llu,\"attitude_valid\":%u,\"roll\":%.7g,\"pitch\":%.7g,\"yaw\":%.7g,"
        "\"gps_valid\":%u,\"gps_fix\":%u,\"gps_sats\":%u,\"lat\":%.10g,\"lon\":%.10g,"
        "\"alt_m\":%.7g,\"rel_alt_m\":%.7g,\"vx_cms\":%d,\"vy_cms\":%d,\"vz_cms\":%d,"
        "\"hdg_cdeg\":%u}\n",
        seq,
        (unsigned long long)ts_ms, telemetry->attitude_valid,
        telemetry->roll, telemetry->pitch, telemetry->yaw,
        telemetry->gps_valid, telemetry->gps_fix_type, telemetry->gps_satellites,
        latitude, longitude, altitude, telemetry->relative_altitude_mm / 1000.0,
        telemetry->velocity_x_cms, telemetry->velocity_y_cms,
        telemetry->velocity_z_cms, telemetry->ground_course_cdeg);
    if (written < 0 || (size_t)written >= packet_size)
        return 0;
    return (size_t)written;
}

/*
 * LoRa uses a compact fixed-size binary packet instead of the LTE JSON.
 *
 * LTE keeps the human-readable JSON packet for UDP, while LoRa carries the
 * same telemetry values in a 46-byte binary frame to reduce airtime on the
 * E220 link.
 *
 * Both links share the same seq and ts_ms, so packets received through LTE
 * and LoRa can be matched directly on the ground.
 *
 * Frame layout:
 *
 *   magic       2 B   0xC5 0x5A
 *   version     1 B
 *   flags       1 B   bit0=attitude valid, bit1=GPS valid
 *   seq         4 B
 *   ts_ms       8 B
 *   roll        2 B   radians * 10000
 *   pitch       2 B   radians * 10000
 *   yaw         2 B   radians * 10000
 *   gps_fix     1 B
 *   gps_sats    1 B
 *   latitude    4 B   degrees * 1e7
 *   longitude   4 B   degrees * 1e7
 *   altitude    2 B   decimetres
 *   rel_alt     2 B   decimetres
 *   vx/vy/vz    6 B   cm/s
 *   heading     2 B   centidegrees
 *   CRC16       2 B   CRC-16/CCITT-FALSE
 *
 * Total: 46 bytes.
 */

#define LORA_PACKET_SIZE 46

static size_t make_lora_packet(
    uint8_t *packet,
    size_t packet_size,
    const MavlinkTelemetry *telemetry,
    uint32_t seq,
    uint64_t ts_ms)
{
    if (packet_size < LORA_PACKET_SIZE)
        return 0;

    size_t i = 0;

    /* magic */
    packet[i++] = 0xC5;
    packet[i++] = 0x5A;

    /* protocol version */
    packet[i++] = 1;

    /* flags */
    uint8_t flags = 0;

    if (telemetry->attitude_valid)
        flags |= 0x01;

    if (telemetry->gps_valid)
        flags |= 0x02;

    packet[i++] = flags;

    /* common sequence */
    put_u32_le(packet + i, seq);
    i += 4;

    /* same timestamp as LTE */
    put_u64_le(packet + i, ts_ms);
    i += 8;

    /*
     * attitude:
     * radian * 10000
     * resolution = 0.0001 rad
     */
    put_i16_le(
        packet + i,
        (int16_t)(telemetry->roll * 10000.0)
    );
    i += 2;

    put_i16_le(
        packet + i,
        (int16_t)(telemetry->pitch * 10000.0)
    );
    i += 2;

    put_i16_le(
        packet + i,
        (int16_t)(telemetry->yaw * 10000.0)
    );
    i += 2;

    /* GPS state */
    packet[i++] = telemetry->gps_fix_type;
    packet[i++] = telemetry->gps_satellites;

    /*
     * latitude / longitude:
     * Pixhawk value already uses degree * 1e7
     */
    put_i32_le(
        packet + i,
        telemetry->latitude_e7
    );
    i += 4;

    put_i32_le(
        packet + i,
        telemetry->longitude_e7
    );
    i += 4;

    /*
    * altitude:
    * millimetres -> decimetres
    * resolution: 0.1 m
    * int16 range: approximately -3276.8 .. +3276.7 m
    */
    put_i16_le(
        packet + i,
        (int16_t)(telemetry->altitude_mm / 100)
    );
    i += 2;

    put_i16_le(
        packet + i,
        (int16_t)(telemetry->relative_altitude_mm / 100)
    );
    i += 2;

    /* velocity is already cm/s */
    put_i16_le(
        packet + i,
        telemetry->velocity_x_cms
    );
    i += 2;

    put_i16_le(
        packet + i,
        telemetry->velocity_y_cms
    );
    i += 2;

    put_i16_le(
        packet + i,
        telemetry->velocity_z_cms
    );
    i += 2;

    /* heading is already centidegrees */
    put_u16_le(
        packet + i,
        telemetry->ground_course_cdeg
    );
    i += 2;

    /*
     * CRC covers everything except CRC itself.
     */
    const uint16_t crc =
        crc16_ccitt(packet, i);

    put_u16_le(packet + i, crc);
    i += 2;

    return i;
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

    /*
	* LTE and LoRa use independent send rates.
	*
	* LTE can run much faster, while LoRa is intentionally rate-limited because
	* the E220 air link is much slower. LoRa now uses a 46-byte binary frame, so
	* rates above the previous 1 Hz limit can be tested without changing the LTE
	* telemetry rate.
	*
	* GCS_LORA_HZ controls the LoRa rate, but LoRa can never run faster than the
	* main telemetry sample rate (send_hz).
    */
    double lora_hz = getenv("GCS_LORA_HZ") ? atof(getenv("GCS_LORA_HZ")) : 1.0;
    if (!(lora_hz > 0.0))
        lora_hz = 1.0;
    if (lora_hz > send_hz)
        lora_hz = send_hz;
    const uint64_t lora_interval_ms = (uint64_t)(1000.0 / lora_hz + 0.5);

    /* The send timer can only fire when poll returns, so the poll timeout
       is computed each pass from the time left until the next deadline
       rather than being a fixed slice of the interval.
     *
     * A fixed slice quantises the send instants: at 50 Hz the old
     * interval/2 gave a 10 ms grain, and asking for 50 Hz produced 47.
     * Waiting exactly as long as the deadline needs removes that.
     *
     * The cap keeps us waking often enough to service the 1 Hz heartbeat
     * and the stream re-request even when the send interval is long. */
    const int max_poll_ms = 250;

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

    const char *lora_path = lora_device();
    const int lora_baud = getenv("GCS_LORA_BAUD") ?
        atoi(getenv("GCS_LORA_BAUD")) : 9600;
    int lora_fd = -1;

    if (lora_path) {
        lora_fd = open_lora(lora_path, lora_baud);

        if (lora_fd < 0) {
            fprintf(
                stderr,
                "gcs: warning: LoRa disabled\n"
            );
        } else {
            fprintf(stdout, "gcs: LoRa send=%g Hz\n", lora_hz);
            fflush(stdout);
        }
    } else {
        fprintf(stdout, "gcs: LoRa off (GCS_LORA_DEVICE unset)\n");
        fflush(stdout);
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    DataReader reader;
    memset(&reader, 0, sizeof(reader));
    reader.connection.fd = -1;
    fprintf(stdout, "gcs: Pixhawk=%s baud=%d UDP=%s send=%g Hz\n",
            device, baud, destination_text, send_hz);
    fflush(stdout);

    /* Deadline for the next send, not the time of the last one: advancing
       it by exactly one interval keeps the long-run rate honest instead of
       letting each pass's overshoot accumulate. 0 = send on the first pass. */
    uint64_t next_report = 0;
    uint64_t last_lora_report = 0;
    unsigned long long lora_sent = 0;
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

        /* Wait only until the next send is due. */
        const uint64_t before_poll = unix_time_ms();
        int poll_ms = 0;
        if (next_report > before_poll) {
            const uint64_t remaining = next_report - before_poll;
            poll_ms = (remaining > (uint64_t)max_poll_ms)
                          ? max_poll_ms : (int)remaining;
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

        if (now >= next_report) {
                const unsigned long long seq =
        		telemetry_seq++;

    		const uint64_t packet_ts_ms = now;

    		const MavlinkTelemetry *telemetry =
        		data_reader_telemetry(&reader);

    		char packet[768];

    		const size_t packet_size = make_packet(
        		packet,
        		sizeof(packet),
        		telemetry,
        		seq,
        		packet_ts_ms
		);
            /* The onboard log keeps JSON: it lands on the SD card, costs
               no airtime, and stays greppable after a flight. */
            if (packet_size > 0)
                log_write(packet, packet_size);

            /* One binary frame per cycle, shared by both radios.
             *
             * LTE used to carry the JSON text -- 262 B where the same
             * values fit in 46. Sending the LoRa frame over UDP as well
             * means one wire format, one parser, and one CRC to trust on
             * the ground, and packets received on either link can still be
             * matched on seq because both are built from this frame. */
            uint8_t frame[LORA_PACKET_SIZE];

            const size_t frame_size = make_lora_packet(
                frame,
                sizeof(frame),
                telemetry,
                (uint32_t)seq,
                packet_ts_ms
            );

            if (frame_size == 0) {
                fprintf(stderr, "gcs: packet build failed\n");
            } else {
                if (sendto(udp_fd, frame, frame_size, 0,
                           (const struct sockaddr *)&destination,
                           sizeof(destination)) < 0) {
                    fprintf(stderr, "gcs: UDP send failed: %s\n",
                            strerror(errno));
                }

                if (lora_fd >= 0 &&
                    now - last_lora_report >= lora_interval_ms) {

                    last_lora_report = now;

                    if (lora_write_all(
                            lora_fd,
                            (const char *)frame,
                            frame_size
                    ) != 0) {
                        fprintf(
                            stderr,
                            "gcs: LoRa send failed: %s\n",
                            strerror(errno)
                        );
                    } else {
                        lora_sent++;
                    }
                }
            }
            /* Keep the cadence, but resynchronise instead of trying to
               catch up if we ever fall a whole interval behind. */
            if (next_report == 0 || now - next_report >= send_interval_ms)
                next_report = now + send_interval_ms;
            else
                next_report += send_interval_ms;
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
                if (lora_fd >= 0)
                    fprintf(stdout, "LoRa=%llu ", lora_sent);
                fputc('\n', stdout);
                fflush(stdout);
                last_console_report = now;
            }
        }
    }

    data_reader_close(&reader);
    close(udp_fd);
    if (lora_fd >= 0)
        close(lora_fd);
    log_close();
    fprintf(stdout, "gcs: stopped\n");
    return 0;
}
