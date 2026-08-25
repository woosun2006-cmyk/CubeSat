#define _DEFAULT_SOURCE
#include "VPN.h"

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_program(const char *program, const char *action, const char *argument)
{
    if (!program || !action || !argument) {
        errno = EINVAL;
        return -1;
    }
    const pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execl(program, program, action, argument, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    errno = EIO;
    return -1;
}

int vpn_up(const char *config_path)
{
    return run_program("/usr/bin/wg-quick", "up", config_path);
}

int vpn_down(const char *config_path)
{
    return run_program("/usr/bin/wg-quick", "down", config_path);
}

int vpn_is_up(const char *interface_name)
{
    if (!interface_name || if_nametoindex(interface_name) == 0)
        return 0;
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0)
        return 0;
    int up = 0;
    for (const struct ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
        if (entry->ifa_name && strcmp(entry->ifa_name, interface_name) == 0 &&
            entry->ifa_flags & IFF_UP) {
            up = 1;
            break;
        }
    }
    freeifaddrs(interfaces);
    return up;
}

#ifdef VPN_STANDALONE
int main(int argc, char **argv)
{
    const char *config_path = argc > 2 ? argv[2] : "/etc/wireguard/wg0.conf";
    const char *action = argc > 1 ? argv[1] : "status";
    if (strcmp(action, "up") == 0 || strcmp(action, "down") == 0) {
        const int result = strcmp(action, "up") == 0 ? vpn_up(config_path) : vpn_down(config_path);
        if (result != 0) {
            fprintf(stderr, "VPN: wg-quick %s failed for %s: %s\n",
                    action, config_path, strerror(errno));
            return 1;
        }
        printf("VPN: wg-quick %s succeeded for %s\n", action, config_path);
        return 0;
    }
    const char *interface_name = argc > 1 ? argv[1] : "wg0";
    printf("VPN: interface %s is %s\n", interface_name,
           vpn_is_up(interface_name) ? "UP" : "DOWN");
    return vpn_is_up(interface_name) ? 0 : 1;
}
#endif
