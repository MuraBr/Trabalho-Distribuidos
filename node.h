#ifndef NODE_H
#define NODE_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NODE_ID_HEX_SIZE ((NODE_ID_SIZE * 2U) + 1U)
#define NODE_UUID_HEX_SIZE ((NODE_UUID_SIZE * 2U) + 1U)

typedef struct
{
    uint8_t bytes[NODE_ID_SIZE];
} NodeID;

typedef struct
{
    char ip[NODE_ADDRESS_SIZE];
    uint16_t port;
    uint8_t uuid[NODE_UUID_SIZE];
} NodeConfig;

typedef enum
{
    NODE_ROLE_PEER = 0,
    NODE_ROLE_SUPERPEER = 1
} NodeRole;

typedef struct
{
    NodeID id;
    NodeConfig config;
    pid_t process_id;
    NodeRole role;
} Node;

// Inicialização de configuração com IP, porta e UUID gerado aleatoriamente.
int node_config_init(NodeConfig *config, const char *ip, uint16_t port);

// Inicialização de configuração com IP, porta e UUID fornecido pelo chamador.
int node_config_init_with_uuid(NodeConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE]);

// Gera um UUID aleatório de 16 bytes, retornando zero em sucesso e -1 em erro.
int node_generate_uuid(uint8_t uuid[NODE_UUID_SIZE]);

// Valida a configuração do nó, incluindo IP, porta e UUID.
int node_config_validate(const NodeConfig *config);

// Calcula o NodeID a partir da configuração do nó, usando SHA-256 de IP binário + porta em ordem de rede + UUID.
int node_compute_id(const NodeConfig *config, NodeID *id);

// Inicializa o nó com a configuração fornecida, calculando o NodeID e registrando o PID.
int node_init(Node *node, const NodeConfig *config);

// Valida o nó, verificando consistência do NodeID, PID e papel; não autentica.
int node_validate(const Node *node);

// Converte o NodeID binário em uma string hexadecimal; retorna -1 em erro.
int node_id_to_hex(const NodeID *id, char *output, size_t output_size);

// Converte uma string hexadecimal em NodeID binário; retorna -1 em erro.
int node_id_from_hex(NodeID *id, const char *hex);

// Compara dois NodeIDs lexicograficamente; retorna -1, 0 ou 1.
int node_id_compare(const NodeID *left, const NodeID *right);

// Compara dois NodeIDs para igualdade; ponteiros nulos não são considerados iguais.
int node_id_equal(const NodeID *left, const NodeID *right);

// Retorna o PID armazenado no nó; retorna -1 se o nó for nulo.
pid_t node_get_process_id(const Node *node);

#endif
