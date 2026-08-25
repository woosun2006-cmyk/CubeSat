#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <ncurses.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int attitude_valid;
    double roll;
    double pitch;
    double yaw;
    int gps_valid;
    int gps_fix;
    int gps_sats;
    double latitude;
    double longitude;
    double altitude;
    double relative_altitude;
    int velocity_x;
    int velocity_y;
    int velocity_z;
    int heading;
    unsigned long long timestamp_ms;
    unsigned long long received_ms;
} Telemetry;

static unsigned long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ull +
           (unsigned long long)now.tv_nsec / 1000000ull;
}

static const char *json_field(const char *json, const char *name)
{
    static char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", name);
    const char *field = strstr(json, needle);
    if (!field)
        return NULL;
    field = strchr(field + strlen(needle), ':');
    return field ? field + 1 : NULL;
}

static int json_double(const char *json, const char *name, double *value)
{
    const char *field = json_field(json, name);
    if (!field)
        return -1;
    char *end = NULL;
    const double parsed = strtod(field, &end);
    if (end == field)
        return -1;
    *value = parsed;
    return 0;
}

static int json_integer(const char *json, const char *name, int *value)
{
    double number;
    if (json_double(json, name, &number) != 0)
        return -1;
    *value = (int)number;
    return 0;
}

static int json_bool(const char *json, const char *name, int *value)
{
    const char *field = json_field(json, name);
    if (!field)
        return -1;
    if (strncmp(field, "true", 4) == 0) {
        *value = 1;
        return 0;
    }
    if (strncmp(field, "false", 5) == 0) {
        *value = 0;
        return 0;
    }
    if (field[0] == '1') {
        *value = 1;
        return 0;
    }
    if (field[0] == '0') {
        *value = 0;
        return 0;
    }
    return -1;
}

static int parse_telemetry(const char *json, Telemetry *telemetry)
{
    Telemetry parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (json_double(json, "ts_ms", &parsed.roll) != 0)
        return -1;
    double timestamp;
    if (json_double(json, "ts_ms", &timestamp) == 0)
        parsed.timestamp_ms = (unsigned long long)timestamp;
    (void)json_bool(json, "attitude_valid", &parsed.attitude_valid);
    (void)json_double(json, "roll", &parsed.roll);
    (void)json_double(json, "pitch", &parsed.pitch);
    (void)json_double(json, "yaw", &parsed.yaw);
    (void)json_bool(json, "gps_valid", &parsed.gps_valid);
    (void)json_integer(json, "gps_fix", &parsed.gps_fix);
    (void)json_integer(json, "gps_sats", &parsed.gps_sats);
    (void)json_double(json, "lat", &parsed.latitude);
    (void)json_double(json, "lon", &parsed.longitude);
    (void)json_double(json, "alt_m", &parsed.altitude);
    (void)json_double(json, "rel_alt_m", &parsed.relative_altitude);
    (void)json_integer(json, "vx_cms", &parsed.velocity_x);
    (void)json_integer(json, "vy_cms", &parsed.velocity_y);
    (void)json_integer(json, "vz_cms", &parsed.velocity_z);
    (void)json_integer(json, "hdg_cdeg", &parsed.heading);
    parsed.received_ms = monotonic_ms();
    *telemetry = parsed;
    return 0;
}

static void interface_info(const char *name, char *state, size_t state_size,
                           char *address, size_t address_size)
{
    snprintf(state, state_size, "DOWN");
    snprintf(address, address_size, "-");
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name);
    FILE *file = fopen(path, "r");
    if (file) {
        if (fgets(state, (int)state_size, file)) {
            state[strcspn(state, "\r\n")] = '\0';
        }
        fclose(file);
    }
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0)
        return;
    for (const struct ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || strcmp(entry->ifa_name, name) != 0 ||
            entry->ifa_addr->sa_family != AF_INET)
            continue;
        const struct sockaddr_in *socket_address =
            (const struct sockaddr_in *)entry->ifa_addr;
        inet_ntop(AF_INET, &socket_address->sin_addr, address, address_size);
        break;
    }
    freeifaddrs(interfaces);
}

static void draw_screen(const Telemetry *telemetry, int port)
{
    char usb_state[16], usb_address[INET_ADDRSTRLEN];
    char wg_state[16], wg_address[INET_ADDRSTRLEN];
    interface_info("usb0", usb_state, sizeof(usb_state),
                   usb_address, sizeof(usb_address));
    interface_info("wg0", wg_state, sizeof(wg_state),
                   wg_address, sizeof(wg_address));

    const unsigned long long age = telemetry->received_ms ?
        monotonic_ms() - telemetry->received_ms : 0;
    const int link_online = telemetry->received_ms && age < 2500;
    int rows, columns;
    getmaxyx(stdscr, rows, columns);
    erase();
    if (rows < 18 || columns < 70) {
        mvprintw(0, 0, "Terminal too small: need at least 70x18 (current %dx%d)",
                 columns, rows);
        mvprintw(2, 0, "Resize the terminal. Press q to quit.");
        refresh();
        return;
    }

    attron(A_BOLD);
    mvprintw(0, 2, "CUBESAT GCS");
    attroff(A_BOLD);
    mvprintw(0, columns - 28, "UDP :%d   q:quit", port);
    mvhline(1, 0, ACS_HLINE, columns);

    mvprintw(3, 2, "TELEMETRY");
    if (link_online)
        attron(COLOR_PAIR(1) | A_BOLD);
    else
        attron(COLOR_PAIR(3) | A_BOLD);
    mvprintw(3, 15, "%s", link_online ? "ONLINE" : "NO DATA");
    attroff(COLOR_PAIR(1) | COLOR_PAIR(3) | A_BOLD);
    mvprintw(4, 2, "Last packet: %s", telemetry->received_ms ?
             (age < 2500 ? "just now" : "STALE") : "never");
    if (telemetry->received_ms)
        mvprintw(4, 30, "age=%llums", age);

    mvprintw(6, 2, "ATTITUDE (rad)");
    if (telemetry->attitude_valid) {
        mvprintw(7, 4, "Roll : %+.6f", telemetry->roll);
        mvprintw(8, 4, "Pitch: %+.6f", telemetry->pitch);
        mvprintw(9, 4, "Yaw  : %+.6f", telemetry->yaw);
    } else {
        attron(COLOR_PAIR(2));
        mvprintw(8, 4, "NO DATA");
        attroff(COLOR_PAIR(2));
    }

    mvprintw(6, 30, "GPS");
    if (telemetry->gps_valid) {
        attron(COLOR_PAIR(1));
        mvprintw(7, 32, "FIX=%d  sats=%d", telemetry->gps_fix, telemetry->gps_sats);
        attroff(COLOR_PAIR(1));
        mvprintw(8, 32, "Lat: %.7f", telemetry->latitude);
        mvprintw(9, 32, "Lon: %.7f", telemetry->longitude);
        mvprintw(10, 32, "Alt: %.2f m", telemetry->altitude);
    } else {
        attron(COLOR_PAIR(2));
        mvprintw(8, 32, "NO FIX");
        attroff(COLOR_PAIR(2));
        mvprintw(9, 32, "GPS not connected or no lock");
    }

    mvprintw(6, 60, "NETWORK");
    mvprintw(7, 62, "LTE usb0: %-8s", usb_state);
    mvprintw(8, 62, "IP: %s", usb_address);
    mvprintw(9, 62, "VPN wg0: %-8s", wg_state);
    mvprintw(10, 62, "IP: %s", wg_address);
    mvhline(12, 0, ACS_HLINE, columns);
    mvprintw(14, 2, "Velocity: vx=%d vy=%d vz=%d cm/s",
             telemetry->velocity_x, telemetry->velocity_y,
             telemetry->velocity_z);
    mvprintw(15, 2, "Heading: %.2f deg", telemetry->heading / 100.0);
    mvprintw(17, 2, "Press q to quit");
    refresh();
}

int main(int argc, char **argv)
{
    const int port = argc > 1 ? atoi(argv[1]) :
        (getenv("GUI_PORT") ? atoi(getenv("GUI_PORT")) : 14550);
    if (port < 1 || port > 65535) {
        fprintf(stderr, "GUI: invalid UDP port %d\n", port);
        return 2;
    }
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("GUI: socket");
        return 1;
    }
    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("GUI: bind");
        close(socket_fd);
        return 1;
    }
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN, -1);
        init_pair(2, COLOR_YELLOW, -1);
        init_pair(3, COLOR_RED, -1);
    }

    Telemetry telemetry;
    memset(&telemetry, 0, sizeof(telemetry));
    int running = 1;
    while (running) {
        struct pollfd descriptor = { socket_fd, POLLIN, 0 };
        (void)poll(&descriptor, 1, 100);
        if (descriptor.revents & POLLIN) {
            char packet[2048];
            ssize_t received;
            while ((received = recv(socket_fd, packet, sizeof(packet) - 1,
                                    MSG_DONTWAIT)) > 0) {
                packet[received] = '\0';
                (void)parse_telemetry(packet, &telemetry);
            }
        }
        const int key = getch();
        if (key == 'q' || key == 'Q')
            running = 0;
        draw_screen(&telemetry, port);
    }
    endwin();
    close(socket_fd);
    return 0;
}
