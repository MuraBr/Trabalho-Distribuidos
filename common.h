#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>

/* Constantes compartilhadas entre a identidade do no e o protocolo. */
#define NODE_ID_SIZE 32U
#define NODE_UUID_SIZE 16U
#define NODE_ADDRESS_SIZE INET6_ADDRSTRLEN

#endif /* COMMON_H */
