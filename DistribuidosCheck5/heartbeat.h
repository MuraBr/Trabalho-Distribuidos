#ifndef HEARTBEAT_H
#define HEARTBEAT_H
#include "superpeer.h"
#include "protocol.h"
#define HEARTBEAT_WIRE_SIZE 187U
typedef struct Heartbeat Heartbeat;
typedef void (*MembershipFailure)(void *context, const SuperPeerMember *member);
typedef int (*PeerReconnect)(void *context);
int heartbeat_create(const Node *local, SuperPeer *members, const char *directory, MembershipFailure failure, void *context, Heartbeat **output);
int heartbeat_start(Heartbeat *heartbeat);
void heartbeat_stop(Heartbeat *heartbeat);
void heartbeat_destroy(Heartbeat *heartbeat);
int heartbeat_handle(Heartbeat *heartbeat, const Message *request, Message_Type *type, uint8_t **payload, uint32_t *size);
int heartbeat_register(Heartbeat *heartbeat, const Node *node, uint64_t incarnation);
int heartbeat_discover(Heartbeat *heartbeat, const NodeConfig *config);
int heartbeat_peer_target(Heartbeat *heartbeat, const NodeConfig *config, PeerReconnect reconnect);
uint64_t heartbeat_incarnation(Heartbeat *heartbeat);
unsigned heartbeat_budget(const Heartbeat *heartbeat);
void heartbeat_gate_lock(Heartbeat *heartbeat);
void heartbeat_gate_unlock(Heartbeat *heartbeat);
int heartbeat_checkpoint(Heartbeat *heartbeat);
#endif
