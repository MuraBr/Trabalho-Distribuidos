#ifndef SUPERPEER_H
#define SUPERPEER_H

#include "node.h"

#include <stddef.h>
#include <time.h>

#define SUPERPEER_DEFAULT_MEMBER_CAPACITY 16U

typedef enum
{
    SUPERPEER_MEMBER_ALIVE = 0
} SuperPeerMemberState;

typedef struct
{
    Node node;
    SuperPeerMemberState state;
    time_t last_seen;
} SuperPeerMember;

typedef struct
{
    NodeConfig node;
    size_t initial_member_capacity;
} SuperPeerConfig;

typedef struct SuperPeer SuperPeer;

typedef enum
{
    SUPERPEER_REGISTER_ERROR = -1,
    SUPERPEER_MEMBER_UPDATED = 0,
    SUPERPEER_MEMBER_ADDED = 1
} SuperPeerRegistrationResult;

/* Initializes a Super Peer configuration with a generated UUID. */
int superpeer_config_init(SuperPeerConfig *config, const char *ip, uint16_t port);

/* Initializes a Super Peer configuration with a caller-provided UUID. */
int superpeer_config_init_with_uuid(SuperPeerConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE]);

/* Creates a Super Peer and registers its local node in its member table. */
int superpeer_create(const SuperPeerConfig *config, SuperPeer **output);

/*
 * Releases the Super Peer and its member table. Callers must stop and join
 * worker threads that use the object before calling this function.
 */
void superpeer_destroy(SuperPeer *superpeer);

/* Copies the local Super Peer node into output. */
int superpeer_get_node(const SuperPeer *superpeer, Node *output);

/* Adds a member or refreshes the existing member with the same NodeID. */
SuperPeerRegistrationResult superpeer_register_node(SuperPeer *superpeer, const Node *node);

/* Removes a non-local member identified by its NodeID. */
int superpeer_unregister_node(SuperPeer *superpeer, const NodeID *node_id);

/* Copies a registered member into output. */
int superpeer_find_member(const SuperPeer *superpeer, const NodeID *node_id, SuperPeerMember *output);

/* Returns the current number of registered members. */
size_t superpeer_member_count(const SuperPeer *superpeer);

/* Returns one when node_id is registered and zero otherwise. */
int superpeer_is_registered(const SuperPeer *superpeer, const NodeID *node_id);

#endif
