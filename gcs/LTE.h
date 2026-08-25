#ifndef CUBESAT_LTE_H
#define CUBESAT_LTE_H

#include <stddef.h>

typedef struct {
    const char *interface_name;
    unsigned int wait_seconds;
} LteConfig;

int lte_interface_up(const char *interface_name);
int lte_get_ipv4(const char *interface_name, char *address, size_t address_size);
int lte_wait_for_ipv4(const LteConfig *config, char *address, size_t address_size);

#endif
