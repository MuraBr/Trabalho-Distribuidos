#ifndef PEER_SERVICE_H
#define PEER_SERVICE_H

#include <stdint.h>

int peer_service_run(uint16_t local_port, const char *superpeer_host, uint16_t superpeer_port);

#endif
