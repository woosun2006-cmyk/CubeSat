#ifndef CUBESAT_VPN_H
#define CUBESAT_VPN_H

int vpn_up(const char *config_path);
int vpn_down(const char *config_path);
int vpn_is_up(const char *interface_name);

#endif
