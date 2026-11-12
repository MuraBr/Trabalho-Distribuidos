#ifndef SUPERPEER_H
#define SUPERPEER_H

#include "node.h"

#include <stddef.h>
#include <time.h>

#define SUPERPEER_DEFAULT_MEMBER_CAPACITY 16U

typedef enum
{
    SUPERPEER_MEMBER_ALIVE = 0,
    SUPERPEER_MEMBER_SUSPECT = 1,
    SUPERPEER_MEMBER_FAILED = 2,
    SUPERPEER_MEMBER_REMOVED = 3
} SuperPeerMemberState;

typedef struct
{
    Node node;
    SuperPeerMemberState state;
    time_t last_seen;
    NodeID home;
    NodeID reporter;
    uint64_t incarnation, sequence, version;
    int64_t observed_ms, probe_ms;
    unsigned failed_probes;
    int local_registration;
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

/* Saída lógica: conserva tombstone e exclui o membro das consultas de registro ativo. */
int superpeer_unregister_node(SuperPeer *superpeer, const NodeID *node_id);

/* Copies a registered member into output. */
int superpeer_find_member(const SuperPeer *superpeer, const NodeID *node_id, SuperPeerMember *output);

/* Conta membros locais ALIVE/SUSPECT, incluindo o nó local; tombstones ficam no snapshot C3. */
size_t superpeer_member_count(const SuperPeer *superpeer);

/* Returns one when node_id is registered and zero otherwise. */
int superpeer_is_registered(const SuperPeer *superpeer, const NodeID *node_id);

/* API C3: snapshots independentes e tempo monotônico fornecido pelo detector. */
SuperPeerRegistrationResult membership_register(SuperPeer *superpeer, const Node *node, const NodeID *home, uint64_t incarnation, int64_t now);
int membership_heartbeat(SuperPeer *superpeer, const SuperPeerMember *member, int64_t now);
int membership_snapshot(SuperPeer *superpeer, SuperPeerMember **output, size_t *count);
int membership_merge(SuperPeer *superpeer, const SuperPeerMember *member);
int membership_probe(SuperPeer *superpeer, const NodeID *id, uint64_t incarnation, uint64_t sequence, int64_t now, int success);
int membership_tick(SuperPeer *superpeer, int64_t now, unsigned suspect_ms, unsigned failed_ms, unsigned removed_ms);
int membership_local(SuperPeer *superpeer, const Node *node, uint64_t incarnation, uint64_t sequence);
int membership_save(SuperPeer *superpeer, const char *path);
int membership_load(SuperPeer *superpeer, const char *path);
const char *membership_state_name(SuperPeerMemberState state);

#endif
