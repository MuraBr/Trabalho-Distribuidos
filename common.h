#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>

/* Constantes compartilhadas entre a identidade do no e o protocolo. */
#define NODE_ID_SIZE 32U /* Resultado binário do SHA-256, compartilhado com o header de rede. */
#define NODE_UUID_SIZE 16U /* Identificador da instância usado na formação do NodeID. */
#define NODE_ADDRESS_SIZE INET6_ADDRSTRLEN /* Espaço para IP textual IPv6 e terminador zero. */

#endif /* COMMON_H */
