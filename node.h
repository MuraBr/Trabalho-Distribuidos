#ifndef NODE_H
#define NODE_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NODE_ID_SIZE 32U
#define NODE_ID_HEX_SIZE ((NODE_ID_SIZE * 2U) + 1U)
#define NODE_UUID_SIZE 16U
#define NODE_UUID_HEX_SIZE ((NODE_UUID_SIZE * 2U) + 1U)
#define NODE_ADDRESS_SIZE INET6_ADDRSTRLEN

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

/* Creates a valid configuration and assigns a random UUID to it. */
int node_config_init(NodeConfig *config, const char *ip, uint16_t port);

/* Creates a valid configuration with a caller-provided UUID. */
int node_config_init_with_uuid(NodeConfig *config,
                               const char *ip,
                               uint16_t port,
                               const uint8_t uuid[NODE_UUID_SIZE]);

/* Fills uuid with a version 4 UUID generated from the OS random source. */
int node_generate_uuid(uint8_t uuid[NODE_UUID_SIZE]);

/* Returns zero when the configuration has a valid IP address and port. */
int node_config_validate(const NodeConfig *config);

/* Computes SHA-256(IP bytes || port in network order || UUID bytes). */
int node_compute_id(const NodeConfig *config, NodeID *id);

/* Initializes a node and records the current process identifier. */
int node_init(Node *node, const NodeConfig *config);

/* Returns zero when the node data and its derived NodeID are consistent. */
int node_validate(const Node *node);

/* Converts a binary NodeID to its lowercase hexadecimal representation. */
int node_id_to_hex(const NodeID *id, char *output, size_t output_size);

/* Parses exactly NODE_ID_HEX_SIZE - 1 hexadecimal characters. */
int node_id_from_hex(NodeID *id, const char *hex);

/* Lexicographic comparison of two binary NodeIDs. */
int node_id_compare(const NodeID *left, const NodeID *right);

/* Returns one when both NodeIDs are equal, and zero otherwise. */
int node_id_equal(const NodeID *left, const NodeID *right);

/* Returns the process identifier recorded in node, or -1 for a null node. */
pid_t node_get_process_id(const Node *node);

#endif
