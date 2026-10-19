#ifndef C4_CLUSTER_H
#define C4_CLUSTER_H

#include "c4_log.h"
#include "superpeer.h"
#include "protocol.h"

#define C4_CLUSTER_RPC_TIMEOUT_MS 750U

typedef struct C4Cluster C4Cluster;
typedef int (*C4ApplyEntry)(void *context, const C4LogEntry *entry);

int c4_cluster_create(const Node *local, SuperPeer *members, const char *log_path, C4ApplyEntry apply, void *context, C4Cluster **output);
void c4_cluster_destroy(C4Cluster *cluster);
int c4_cluster_handle(C4Cluster *cluster, const Message *request, Message_Type *response_type, uint8_t **payload, uint32_t *payload_size);
int c4_cluster_tick(C4Cluster *cluster);
int c4_cluster_submit(C4Cluster *cluster, const uint8_t transaction_id[TRANSACTION_ID_SIZE], const NodeID *owner, const uint8_t *payload, uint32_t payload_size);
int c4_cluster_is_leader(C4Cluster *cluster);
int c4_cluster_get_leader(C4Cluster *cluster, NodeID *leader);
uint64_t c4_cluster_last_index(C4Cluster *cluster);

#endif
