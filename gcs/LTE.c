#define _DEFAULT_SOURCE
#include "LTE.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int lte_interface_up(const char *interface_name)
{
    if (!interface_name || if_nametoindex(interface_name) == 0) {
        errno = ENODEV;
        return -1;
    }
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;

    struct ifreq request;
    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, interface_name, IFNAMSIZ - 1);
    if (ioctl(socket_fd, SIOCGIFFLAGS, &request) != 0) {
        close(socket_fd);
        return -1;
    }
    if (!(request.ifr_flags & IFF_UP)) {
        request.ifr_flags |= IFF_UP;
        if (ioctl(socket_fd, SIOCSIFFLAGS, &request) != 0) {
            close(socket_fd);
            return -1;
        }
    }
    close(socket_fd);
    return 0;
}

int lte_get_ipv4(const char *interface_name, char *address, size_t address_size)
{
    if (!interface_name || !address || address_size == 0) {
        errno = EINVAL;
        return -1;
    }
    address[0] = '\0';
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0)
        return -1;
    int result = -1;
    for (const struct ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || strcmp(entry->ifa_name, interface_name) != 0 ||
            entry->ifa_addr->sa_family != AF_INET)
            continue;
        const struct sockaddr_in *socket_address =
            (const struct sockaddr_in *)entry->ifa_addr;
        if (!inet_ntop(AF_INET, &socket_address->sin_addr, address, address_size))
            continue;
        if (strcmp(address, "0.0.0.0") != 0) {
            result = 0;
            break;
        }
    }
    freeifaddrs(interfaces);
    if (result != 0)
        errno = ENETUNREACH;
    return result;
}

int lte_wait_for_ipv4(const LteConfig *config, char *address, size_t address_size)
{
    if (!config || !config->interface_name || config->wait_seconds == 0) {
        errno = EINVAL;
        return -1;
    }
    (void)lte_interface_up(config->interface_name);
    for (unsigned int second = 0; second < config->wait_seconds; ++second) {
        if (lte_get_ipv4(config->interface_name, address, address_size) == 0)
            return 0;
        sleep(1);
    }
    return -1;
}

#ifdef LTE_STANDALONE
int main(int argc, char **argv)
{
    const char *interface_name = argc > 1 ? argv[1] : "wwan0";
    const unsigned int wait_seconds = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 30;
    char address[INET_ADDRSTRLEN];
    const LteConfig config = { interface_name, wait_seconds };
    if (lte_wait_for_ipv4(&config, address, sizeof(address)) != 0) {
        fprintf(stderr, "LTE: no IPv4 address on %s after %u seconds: %s\n",
                interface_name, wait_seconds, strerror(errno));
        return 1;
    }
    printf("LTE: %s IPv4=%s\n", interface_name, address);
    return 0;
}
#endif
