#define _POSIX_C_SOURCE 200809L
#include "c4_cluster.h"

#include "rpc.h"
#include "network.h"
#include "remote_error.h"
#include "wire.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C4_ELECTION_PAYLOAD_SIZE 24U
#define C4_COORDINATOR_PAYLOAD_SIZE 16U
#define C4_FORWARD_OWNER_SIZE NODE_ID_SIZE
#define C4_ELECTION_RETRY_MS 1500
#define C4_RPC_TIMEOUT_MS 750U

typedef struct
{
    uint8_t id[TRANSACTION_ID_SIZE];
    int used;
} C4TransactionIdSlot;

static uint64_t c4_transaction_hash(const uint8_t id[TRANSACTION_ID_SIZE])
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0U; index < TRANSACTION_ID_SIZE; ++index)
    {
        hash ^= id[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

struct C4Cluster
{
    Node local;
    SuperPeer *members;
    C4Log *log;
    C4ApplyEntry apply;
    void *context;
    pthread_mutex_t mutex;
    NodeID leader;
    uint64_t term;
    int election_requested;
    int election_waiting;
    int64_t election_started_ms;
};

static int c4_id_is_zero(const uint8_t id[NODE_ID_SIZE])
{
    static const uint8_t zero[NODE_ID_SIZE] = {0U};
    return memcmp(id, zero, NODE_ID_SIZE) == 0;
}

static int c4_alive_superpeers(C4Cluster *cluster, SuperPeerMember **members, size_t *count)
{
    SuperPeerMember *snapshot = NULL;
    size_t snapshot_count = 0U;
    if (membership_snapshot(cluster->members, &snapshot, &snapshot_count) < 0)
    {
        return -1;
    }
    *members = snapshot;
    *count = snapshot_count;
    return 0;
}

static size_t c4_count_live_superpeers(const C4Cluster *cluster, const SuperPeerMember *members, size_t count)
{
    size_t index;
    size_t alive_count = 1U;
    for (index = 0U; index < count; ++index)
    {
        if (members[index].node.role == NODE_ROLE_SUPERPEER && members[index].state < SUPERPEER_MEMBER_FAILED && !node_id_equal(&members[index].node.id, &cluster->local.id)) ++alive_count;
    }
    return alive_count;
}

static int c4_member_is_live_superpeer(C4Cluster *cluster, const uint8_t id[NODE_ID_SIZE], SuperPeerMember *output)
{
    NodeID node_id;
    SuperPeerMember member;
    memcpy(node_id.bytes, id, NODE_ID_SIZE);
    if (superpeer_find_member(cluster->members, &node_id, &member) < 0 || member.node.role != NODE_ROLE_SUPERPEER || member.state >= SUPERPEER_MEMBER_FAILED)
    {
        errno = EACCES;
        return -1;
    }
    if (output != NULL) *output = member;
    return 0;
}

static int c4_rpc(C4Cluster *cluster, const SuperPeerMember *member, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message *response)
{
    NodeID destination = member->node.id;
    return rpc_call_deadline(member->node.config.ip, member->node.config.port, &cluster->local.id, &destination, type, payload, payload_size, response, C4_RPC_TIMEOUT_MS);
}

static int c4_rpc_transaction(C4Cluster *cluster, const SuperPeerMember *member, Message_Type type, const uint8_t transaction_id[TRANSACTION_ID_SIZE], const uint8_t *payload, uint32_t payload_size, Message *response)
{
    NodeID destination = member->node.id;
    return rpc_call_transaction_deadline(member->node.config.ip, member->node.config.port, &cluster->local.id, &destination, type, transaction_id, payload, payload_size, response, C4_RPC_TIMEOUT_MS);
}

static int c4_apply_new_entry(C4Cluster *cluster, const C4LogEntry *entry)
{
    return cluster->apply == NULL ? 0 : cluster->apply(cluster->context, entry);
}

static int c4_source_is_active_leader(C4Cluster *cluster, const uint8_t source[NODE_ID_SIZE])
{
    NodeID leader;
    (void)pthread_mutex_lock(&cluster->mutex);
    leader = cluster->leader;
    (void)pthread_mutex_unlock(&cluster->mutex);
    if (c4_id_is_zero(leader.bytes) || memcmp(leader.bytes, source, NODE_ID_SIZE) != 0)
    {
        errno = EACCES;
        return -1;
    }
    return c4_member_is_live_superpeer(cluster, source, NULL);
}

static int c4_recover_from_member(C4Cluster *cluster, const SuperPeerMember *member)
{
    uint64_t last_index = c4_log_last_index(cluster->log);
    uint64_t last_version = c4_log_last_version(cluster->log);
    for (;;)
    {
        Message response;
        uint8_t request_payload[16];
        uint32_t count;
        uint32_t cursor = 4U;
        uint32_t item;
        message_init(&response);
        wire_put_u64(request_payload, last_index);
        wire_put_u64(request_payload + 8U, last_version);
        if (c4_rpc(cluster, member, M_IST_REQUEST, request_payload, sizeof(request_payload), &response) < 0)
        {
            message_free(&response);
            return -1;
        }
        if (response.header.message_type != (uint8_t)M_IST_BATCH || response.header.payload_size < 4U)
        {
            if (response.header.message_type == (uint8_t)M_ERROR) errno = remote_error_decode(response.payload, response.header.payload_size);
            else errno = EBADMSG;
            message_free(&response);
            return -1;
        }
        count = wire_get_u32(response.payload);
        if (count == 0U)
        {
            if (response.header.payload_size != 4U)
            {
                message_free(&response);
                errno = EBADMSG;
                return -1;
            }
            message_free(&response);
            return 0;
        }
        if (count > (response.header.payload_size - 4U) / 48U)
        {
            message_free(&response);
            errno = EBADMSG;
            return -1;
        }
        C4LogEntry *entries = calloc(count, sizeof(*entries));
        size_t seen_capacity = 1U;
        C4TransactionIdSlot *seen;
        uint64_t validated_index = last_index;
        uint64_t validated_version = last_version;
        while (seen_capacity < (size_t)count * 2U) seen_capacity *= 2U;
        seen = calloc(seen_capacity, sizeof(*seen));
        if (entries == NULL || seen == NULL)
        {
            free(entries);
            free(seen);
            message_free(&response);
            return -1;
        }
        for (item = 0U; item < count; ++item)
        {
            uint32_t encoded_size;
            if (cursor > response.header.payload_size - 4U)
            {
                errno = EBADMSG;
                goto ist_batch_invalid;
            }
            encoded_size = wire_get_u32(response.payload + cursor);
            cursor += 4U;
            if (encoded_size > response.header.payload_size - cursor)
            {
                errno = EBADMSG;
                goto ist_batch_invalid;
            }
            if (c4_log_entry_decode(response.payload + cursor, encoded_size, &entries[item]) < 0 || entries[item].index != validated_index + 1U || entries[item].version != validated_version + 1U)
            {
                errno = EBADMSG;
                goto ist_batch_invalid;
            }
            C4LogEntry existing;
            if (c4_log_get_transaction(cluster->log, entries[item].transaction_id, &existing) == 0)
            {
                c4_log_entry_free(&existing);
                errno = EEXIST;
                goto ist_batch_invalid;
            }
            if (errno != ENOENT) goto ist_batch_invalid;
            size_t seen_index = (size_t)c4_transaction_hash(entries[item].transaction_id) & (seen_capacity - 1U);
            while (seen[seen_index].used)
            {
                if (memcmp(seen[seen_index].id, entries[item].transaction_id, TRANSACTION_ID_SIZE) == 0)
                {
                    errno = EEXIST;
                    goto ist_batch_invalid;
                }
                seen_index = (seen_index + 1U) & (seen_capacity - 1U);
            }
            seen[seen_index].used = 1;
            memcpy(seen[seen_index].id, entries[item].transaction_id, TRANSACTION_ID_SIZE);
            validated_index = entries[item].index;
            validated_version = entries[item].version;
            cursor += encoded_size;
        }
        if (cursor != response.header.payload_size)
        {
            errno = EBADMSG;
            goto ist_batch_invalid;
        }
        for (item = 0U; item < count; ++item)
        {
            if (c4_log_append_entry(cluster->log, &entries[item]) < 0 || c4_apply_new_entry(cluster, &entries[item]) < 0) goto ist_batch_invalid;
            last_index = entries[item].index;
            last_version = entries[item].version;
        }
        for (item = 0U; item < count; ++item) c4_log_entry_free(&entries[item]);
        free(entries);
        free(seen);
        message_free(&response);
        continue;
ist_batch_invalid:
        for (item = 0U; item < count; ++item) c4_log_entry_free(&entries[item]);
        free(entries);
        free(seen);
        message_free(&response);
        return -1;
    }
}

static int c4_broadcast_coordinator(C4Cluster *cluster, SuperPeerMember *members, size_t count, uint64_t term)
{
    uint8_t payload[C4_COORDINATOR_PAYLOAD_SIZE];
    size_t index;
    wire_put_u64(payload, term);
    wire_put_u64(payload + 8U, c4_log_last_index(cluster->log));
    for (index = 0U; index < count; ++index)
    {
        Message response;
        if (members[index].node.role != NODE_ROLE_SUPERPEER || members[index].state >= SUPERPEER_MEMBER_FAILED || node_id_equal(&members[index].node.id, &cluster->local.id)) continue;
        message_init(&response);
        if (c4_rpc(cluster, &members[index], M_COORDINATOR, payload, sizeof(payload), &response) == 0 && response.header.message_type == (uint8_t)M_ACK)
        {
            printf("C4 Coordinator confirmado pelo membro %u\n", (unsigned)members[index].node.config.port);
        }
        message_free(&response);
    }
    return 0;
}

static int c4_broadcast_log(C4Cluster *cluster, SuperPeerMember *members, size_t count)
{
    size_t member_index;
    uint64_t last_index = c4_log_last_index(cluster->log);
    for (member_index = 0U; member_index < count; ++member_index)
    {
        if (members[member_index].node.role != NODE_ROLE_SUPERPEER || members[member_index].state >= SUPERPEER_MEMBER_FAILED || node_id_equal(&members[member_index].node.id, &cluster->local.id)) continue;
        for (uint64_t log_index = 1U; log_index <= last_index; ++log_index)
        {
            C4LogEntry entry;
            Message response;
            uint8_t *wire = NULL;
            uint32_t wire_size = 0U;
            int result;
            if (c4_log_get(cluster->log, log_index, &entry) < 0) return -1;
            result = c4_log_entry_encode(&entry, &wire, &wire_size);
            c4_log_entry_free(&entry);
            if (result < 0) return -1;
            message_init(&response);
            result = c4_rpc(cluster, &members[member_index], M_STATE_TRANSFER, wire, wire_size, &response);
            free(wire);
            if (result < 0 || response.header.message_type != (uint8_t)M_ACK)
            {
                message_free(&response);
                return -1;
            }
            message_free(&response);
        }
    }
    return 0;
}

static int c4_become_coordinator(C4Cluster *cluster, SuperPeerMember *members, size_t count)
{
    size_t index;
    uint64_t term;
    for (index = 0U; index < count; ++index)
    {
        if (members[index].node.role == NODE_ROLE_SUPERPEER && members[index].state < SUPERPEER_MEMBER_FAILED && !node_id_equal(&members[index].node.id, &cluster->local.id))
        {
            if (c4_recover_from_member(cluster, &members[index]) < 0 && (errno == EBADMSG || errno == EEXIST || errno == ERANGE))
            {
                (void)pthread_mutex_lock(&cluster->mutex);
                cluster->election_requested = 1;
                cluster->election_waiting = 1;
                cluster->election_started_ms = network_monotonic_ms();
                (void)pthread_mutex_unlock(&cluster->mutex);
                fprintf(stderr, "C4 eleição suspensa: logs replicados divergem.\n");
                return -1;
            }
        }
    }
    (void)pthread_mutex_lock(&cluster->mutex);
    term = ++cluster->term;
    cluster->leader = cluster->local.id;
    cluster->election_waiting = 0;
    cluster->election_requested = 0;
    (void)pthread_mutex_unlock(&cluster->mutex);
    printf("C4 COORDINATOR NodeID=");
    for (index = 0U; index < NODE_ID_SIZE; ++index) printf("%02x", cluster->local.id.bytes[index]);
    printf(" term=%llu index=%llu\n", (unsigned long long)term, (unsigned long long)c4_log_last_index(cluster->log));
    fflush(stdout);
    if (c4_broadcast_coordinator(cluster, members, count, term) < 0) return -1;
    if (c4_broadcast_log(cluster, members, count) < 0) fprintf(stderr, "C4 aviso: não foi possível sincronizar todas as réplicas.\n");
    return 0;
}

int c4_cluster_create(const Node *local, SuperPeer *members, const char *log_path, C4ApplyEntry apply, void *context, C4Cluster **output)
{
    C4Cluster *cluster;
    int mutex_status;
    if (local == NULL || members == NULL || log_path == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    cluster = calloc(1U, sizeof(*cluster));
    if (cluster == NULL) return -1;
    cluster->local = *local;
    cluster->members = members;
    cluster->apply = apply;
    cluster->context = context;
    mutex_status = pthread_mutex_init(&cluster->mutex, NULL);
    if (mutex_status != 0 || c4_log_open(log_path, &cluster->log) < 0)
    {
        int error = mutex_status != 0 ? mutex_status : errno;
        if (mutex_status == 0) (void)pthread_mutex_destroy(&cluster->mutex);
        free(cluster);
        errno = error;
        return -1;
    }
    for (uint64_t index = 1U; index <= c4_log_last_index(cluster->log); ++index)
    {
        C4LogEntry entry;
        memset(&entry, 0, sizeof(entry));
        if (c4_log_get(cluster->log, index, &entry) < 0 || (apply != NULL && apply(context, &entry) < 0))
        {
            if (entry.payload != NULL) c4_log_entry_free(&entry);
            c4_log_close(cluster->log);
            (void)pthread_mutex_destroy(&cluster->mutex);
            free(cluster);
            return -1;
        }
        c4_log_entry_free(&entry);
    }
    cluster->election_requested = 1;
    *output = cluster;
    return 0;
}

void c4_cluster_destroy(C4Cluster *cluster)
{
    if (cluster == NULL) return;
    c4_log_close(cluster->log);
    (void)pthread_mutex_destroy(&cluster->mutex);
    free(cluster);
}

int c4_cluster_handle(C4Cluster *cluster, const Message *request, Message_Type *response_type, uint8_t **payload, uint32_t *payload_size)
{
    SuperPeerMember member;
    if (cluster == NULL || request == NULL || response_type == NULL || payload == NULL || payload_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *payload = NULL;
    *payload_size = 0U;
    *response_type = M_ERROR;
    if (memcmp(request->header.destination_node, cluster->local.id.bytes, NODE_ID_SIZE) != 0)
    {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (request->header.message_type == (uint8_t)M_ELECTION)
    {
        if (request->header.payload_size != C4_ELECTION_PAYLOAD_SIZE || c4_member_is_live_superpeer(cluster, request->header.source_node, &member) < 0) return -1;
        *response_type = M_OK;
        if (memcmp(request->header.source_node, cluster->local.id.bytes, NODE_ID_SIZE) < 0)
        {
            (void)pthread_mutex_lock(&cluster->mutex);
            cluster->election_requested = 1;
            (void)pthread_mutex_unlock(&cluster->mutex);
        }
        return 0;
    }
    if (request->header.message_type == (uint8_t)M_COORDINATOR)
    {
        uint64_t term;
        if (request->header.payload_size != C4_COORDINATOR_PAYLOAD_SIZE || c4_member_is_live_superpeer(cluster, request->header.source_node, &member) < 0) return -1;
        term = wire_get_u64(request->payload);
        (void)pthread_mutex_lock(&cluster->mutex);
        if (term < cluster->term || memcmp(request->header.source_node, cluster->local.id.bytes, NODE_ID_SIZE) < 0)
        {
            (void)pthread_mutex_unlock(&cluster->mutex);
            errno = ESTALE;
            return -1;
        }
        cluster->term = term;
        memcpy(cluster->leader.bytes, request->header.source_node, NODE_ID_SIZE);
        cluster->election_requested = 0;
        cluster->election_waiting = 0;
        (void)pthread_mutex_unlock(&cluster->mutex);
        if (c4_recover_from_member(cluster, &member) < 0)
        {
            (void)pthread_mutex_lock(&cluster->mutex);
            if (memcmp(cluster->leader.bytes, request->header.source_node, NODE_ID_SIZE) == 0) memset(cluster->leader.bytes, 0, NODE_ID_SIZE);
            (void)pthread_mutex_unlock(&cluster->mutex);
            return -1;
        }
        *response_type = M_ACK;
        printf("C4 COORDINATOR aceito NodeID=");
        for (size_t index = 0U; index < NODE_ID_SIZE; ++index) printf("%02x", request->header.source_node[index]);
        printf(" term=%llu\n", (unsigned long long)term);
        fflush(stdout);
        return 0;
    }
    if (request->header.message_type == (uint8_t)M_IST_REQUEST)
    {
        uint64_t requested_index;
        uint64_t requested_version;
        uint64_t last_index;
        uint8_t *batch;
        uint32_t cursor = 4U;
        uint32_t count = 0U;
        if (request->header.payload_size != 16U || c4_member_is_live_superpeer(cluster, request->header.source_node, &member) < 0) return -1;
        requested_index = wire_get_u64(request->payload);
        requested_version = wire_get_u64(request->payload + 8U);
        last_index = c4_log_last_index(cluster->log);
        if (requested_index > last_index || (requested_index == 0U && requested_version != 0U))
        {
            errno = requested_index > last_index ? ENOENT : ERANGE;
            return -1;
        }
        if (requested_index > 0U)
        {
            C4LogEntry previous;
            if (c4_log_get(cluster->log, requested_index, &previous) < 0) return -1;
            if (previous.version != requested_version)
            {
                c4_log_entry_free(&previous);
                errno = ERANGE;
                return -1;
            }
            c4_log_entry_free(&previous);
        }
        batch = calloc(MAX_PAYLOAD_SIZE, 1U);
        if (batch == NULL) return -1;
        for (uint64_t index = requested_index + 1U; index <= last_index; ++index)
        {
            C4LogEntry entry;
            uint8_t *encoded = NULL;
            uint32_t encoded_size = 0U;
            if (c4_log_get(cluster->log, index, &entry) < 0 || c4_log_entry_encode(&entry, &encoded, &encoded_size) < 0)
            {
                c4_log_entry_free(&entry);
                free(encoded);
                free(batch);
                return -1;
            }
            c4_log_entry_free(&entry);
            if (encoded_size > MAX_PAYLOAD_SIZE - cursor - 4U)
            {
                free(encoded);
                if (count == 0U)
                {
                    free(batch);
                    errno = EMSGSIZE;
                    return -1;
                }
                break;
            }
            wire_put_u32(batch + cursor, encoded_size);
            cursor += 4U;
            memcpy(batch + cursor, encoded, encoded_size);
            cursor += encoded_size;
            free(encoded);
            ++count;
        }
        wire_put_u32(batch, count);
        *response_type = M_IST_BATCH;
        *payload = batch;
        *payload_size = cursor;
        return 0;
    }
    if (request->header.message_type == (uint8_t)M_SNAPSHOT)
    {
        C4LogEntry entry;
        uint64_t index;
        if (request->header.payload_size != 8U || c4_member_is_live_superpeer(cluster, request->header.source_node, &member) < 0) return -1;
        index = wire_get_u64(request->payload);
        *response_type = M_ACK;
        if (c4_log_get(cluster->log, index, &entry) == 0)
        {
            int result = c4_log_entry_encode(&entry, payload, payload_size);
            c4_log_entry_free(&entry);
            return result;
        }
        if (errno == ENOENT) return 0;
        return -1;
    }
    if (request->header.message_type == (uint8_t)M_STATE_TRANSFER)
    {
        C4LogEntry entry;
        NodeID leader;
        (void)pthread_mutex_lock(&cluster->mutex);
        leader = cluster->leader;
        (void)pthread_mutex_unlock(&cluster->mutex);
        if (c4_id_is_zero(leader.bytes) || memcmp(leader.bytes, request->header.source_node, NODE_ID_SIZE) != 0)
        {
            errno = EACCES;
            return -1;
        }
        if (c4_member_is_live_superpeer(cluster, request->header.source_node, &member) < 0 || c4_log_entry_decode(request->payload, request->header.payload_size, &entry) < 0) return -1;
        if (c4_log_append_entry(cluster->log, &entry) < 0 || c4_apply_new_entry(cluster, &entry) < 0)
        {
            c4_log_entry_free(&entry);
            return -1;
        }
        c4_log_entry_free(&entry);
        *response_type = M_ACK;
        return 0;
    }
    if (request->header.message_type == (uint8_t)M_PREPARE)
    {
        C4LogEntry entry;
        if (c4_source_is_active_leader(cluster, request->header.source_node) < 0 || c4_log_entry_decode(request->payload, request->header.payload_size, &entry) < 0) return -1;
        if (memcmp(entry.transaction_id, request->header.transaction_id, TRANSACTION_ID_SIZE) != 0 || c4_log_prepare(cluster->log, &entry) < 0)
        {
            c4_log_entry_free(&entry);
            return -1;
        }
        flockfile(stdout);
        printf("C5 PREPARE transaction=");
        for (size_t index = 0U; index < TRANSACTION_ID_SIZE; ++index) printf("%02x", entry.transaction_id[index]);
        printf(" index=%llu version=%llu\n", (unsigned long long)entry.index, (unsigned long long)entry.version);
        fflush(stdout);
        funlockfile(stdout);
        c4_log_entry_free(&entry);
        *response_type = M_ACK;
        return 0;
    }
    if (request->header.message_type == (uint8_t)M_COMMIT || request->header.message_type == (uint8_t)M_ABORT)
    {
        C4LogEntry entry;
        if (request->header.payload_size != 0U || c4_source_is_active_leader(cluster, request->header.source_node) < 0) return -1;
        if (request->header.message_type == (uint8_t)M_ABORT)
        {
            if (c4_log_abort(cluster->log, request->header.transaction_id) < 0) return -1;
        }
        else
        {
            if (c4_log_commit(cluster->log, request->header.transaction_id, &entry) < 0) return -1;
            if (c4_apply_new_entry(cluster, &entry) < 0)
            {
                c4_log_entry_free(&entry);
                return -1;
            }
            c4_log_entry_free(&entry);
        }
        flockfile(stdout);
        printf("C5 %s transaction=", request->header.message_type == (uint8_t)M_COMMIT ? "COMMIT" : "ABORT");
        for (size_t index = 0U; index < TRANSACTION_ID_SIZE; ++index) printf("%02x", request->header.transaction_id[index]);
        printf("\n");
        fflush(stdout);
        funlockfile(stdout);
        *response_type = M_ACK;
        return 0;
    }
    errno = ENOTSUP;
    return -1;
}

int c4_cluster_tick(C4Cluster *cluster)
{
    SuperPeerMember *members = NULL;
    size_t count = 0U;
    size_t index;
    int request_election;
    int waiting;
    int higher_responded = 0;
    int64_t now = network_monotonic_ms();
    if (cluster == NULL || c4_alive_superpeers(cluster, &members, &count) < 0) return -1;
    (void)pthread_mutex_lock(&cluster->mutex);
    if (!c4_id_is_zero(cluster->leader.bytes) && !node_id_equal(&cluster->leader, &cluster->local.id))
    {
        SuperPeerMember leader_member;
        if (superpeer_find_member(cluster->members, &cluster->leader, &leader_member) < 0 || leader_member.state >= SUPERPEER_MEMBER_FAILED)
        {
            memset(cluster->leader.bytes, 0, NODE_ID_SIZE);
            cluster->election_requested = 1;
        }
    }
    request_election = cluster->election_requested;
    if (node_id_equal(&cluster->leader, &cluster->local.id))
    {
        for (index = 0U; index < count; ++index)
        {
            if (members[index].node.role == NODE_ROLE_SUPERPEER && members[index].state < SUPERPEER_MEMBER_FAILED && memcmp(members[index].node.id.bytes, cluster->local.id.bytes, NODE_ID_SIZE) > 0)
            {
                cluster->election_requested = 1;
                request_election = 1;
                break;
            }
        }
    }
    waiting = cluster->election_waiting;
    if (waiting && now - cluster->election_started_ms >= C4_ELECTION_RETRY_MS)
    {
        cluster->election_waiting = 0;
        cluster->election_requested = 1;
        request_election = 1;
        waiting = 0;
    }
    (void)pthread_mutex_unlock(&cluster->mutex);
    if (!request_election)
    {
        free(members);
        return 0;
    }
    printf("C4 ELECTION iniciado NodeID=");
    for (index = 0U; index < NODE_ID_SIZE; ++index) printf("%02x", cluster->local.id.bytes[index]);
    printf(" term=%llu\n", (unsigned long long)cluster->term);
    fflush(stdout);
    for (index = 0U; index < count; ++index)
    {
        uint8_t payload[C4_ELECTION_PAYLOAD_SIZE];
        Message response;
        if (members[index].node.role != NODE_ROLE_SUPERPEER || members[index].state >= SUPERPEER_MEMBER_FAILED || memcmp(members[index].node.id.bytes, cluster->local.id.bytes, NODE_ID_SIZE) <= 0) continue;
        wire_put_u64(payload, cluster->term);
        wire_put_u64(payload + 8U, c4_log_last_index(cluster->log));
        wire_put_u64(payload + 16U, c4_log_last_version(cluster->log));
        message_init(&response);
        if (c4_rpc(cluster, &members[index], M_ELECTION, payload, sizeof(payload), &response) == 0 && response.header.message_type == (uint8_t)M_OK) higher_responded = 1;
        message_free(&response);
    }
    if (higher_responded)
    {
        (void)pthread_mutex_lock(&cluster->mutex);
        cluster->election_waiting = 1;
        cluster->election_requested = 0;
        cluster->election_started_ms = now;
        (void)pthread_mutex_unlock(&cluster->mutex);
    }
    else
    {
        (void)c4_become_coordinator(cluster, members, count);
    }
    free(members);
    return 0;
}

int c4_cluster_submit(C4Cluster *cluster, const uint8_t transaction_id[TRANSACTION_ID_SIZE], const NodeID *owner, const uint8_t *payload, uint32_t payload_size)
{
    SuperPeerMember *members = NULL;
    size_t count = 0U;
    size_t index;
    uint8_t *operation;
    uint8_t *wire = NULL;
    uint32_t wire_size = 0U;
    C4LogEntry entry;
    size_t expected_replicas;
    size_t sent_count = 0U;
    SuperPeerMember **sent_members = NULL;
    int prepare_ok = 1;
    size_t acknowledgements;
    if (cluster == NULL || transaction_id == NULL || owner == NULL || payload == NULL || payload_size == 0U || payload_size > MAX_PAYLOAD_SIZE - C4_FORWARD_OWNER_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    if (!c4_cluster_is_leader(cluster))
    {
        errno = EHOSTUNREACH;
        return -1;
    }
    if (c4_alive_superpeers(cluster, &members, &count) < 0) return -1;
    expected_replicas = c4_count_live_superpeers(cluster, members, count);
    operation = malloc(C4_FORWARD_OWNER_SIZE + (size_t)payload_size);
    if (operation == NULL)
    {
        free(members);
        return -1;
    }
    memcpy(operation, owner->bytes, C4_FORWARD_OWNER_SIZE);
    memcpy(operation + C4_FORWARD_OWNER_SIZE, payload, payload_size);
    memset(&entry, 0, sizeof(entry));
    if (c4_log_get_transaction(cluster->log, transaction_id, &entry) == 0)
    {
        int same = entry.payload_size == C4_FORWARD_OWNER_SIZE + payload_size && memcmp(entry.payload, operation, entry.payload_size) == 0;
        c4_log_entry_free(&entry);
        if (!same)
        {
            free(operation);
            free(members);
            errno = EEXIST;
            return -1;
        }
        acknowledgements = 1U;
        for (index = 0U; index < count; ++index)
        {
            Message response;
            if (members[index].node.role != NODE_ROLE_SUPERPEER || members[index].state >= SUPERPEER_MEMBER_FAILED || node_id_equal(&members[index].node.id, &cluster->local.id)) continue;
            message_init(&response);
            if (c4_rpc_transaction(cluster, &members[index], M_COMMIT, transaction_id, NULL, 0U, &response) == 0 && response.header.message_type == (uint8_t)M_ACK) ++acknowledgements;
            message_free(&response);
        }
        free(operation);
        free(members);
        if (acknowledgements != expected_replicas)
        {
            errno = EHOSTUNREACH;
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) goto submit_fail;
    entry.index = c4_log_last_index(cluster->log) + 1U;
    entry.version = c4_log_last_version(cluster->log) + 1U;
    memcpy(entry.transaction_id, transaction_id, TRANSACTION_ID_SIZE);
    entry.payload = operation;
    entry.payload_size = C4_FORWARD_OWNER_SIZE + payload_size;
    entry.checksum = c4_log_entry_checksum(&entry);
    if (c4_log_prepare(cluster->log, &entry) < 0 || c4_log_entry_encode(&entry, &wire, &wire_size) < 0) goto submit_fail;
    flockfile(stdout);
    printf("C5 PREPARE transaction=");
    for (index = 0U; index < TRANSACTION_ID_SIZE; ++index) printf("%02x", transaction_id[index]);
    printf(" index=%llu version=%llu participants=%zu\n", (unsigned long long)entry.index, (unsigned long long)entry.version, expected_replicas);
    fflush(stdout);
    funlockfile(stdout);
    sent_members = calloc(count, sizeof(*sent_members));
    if (sent_members == NULL && count > 0U) goto submit_abort;
    for (index = 0U; index < count; ++index)
    {
        Message response;
        if (members[index].node.role != NODE_ROLE_SUPERPEER || members[index].state >= SUPERPEER_MEMBER_FAILED || node_id_equal(&members[index].node.id, &cluster->local.id)) continue;
        sent_members[sent_count++] = &members[index];
        message_init(&response);
        if (c4_rpc_transaction(cluster, &members[index], M_PREPARE, transaction_id, wire, wire_size, &response) < 0 || response.header.message_type != (uint8_t)M_ACK) prepare_ok = 0;
        message_free(&response);
    }
    if (!prepare_ok || sent_count + 1U != expected_replicas) goto submit_abort;
    if (c4_log_commit(cluster->log, transaction_id, &entry) < 0 || c4_apply_new_entry(cluster, &entry) < 0) goto submit_fail;
    flockfile(stdout);
    printf("C5 COMMIT transaction=");
    for (index = 0U; index < TRANSACTION_ID_SIZE; ++index) printf("%02x", transaction_id[index]);
    printf(" index=%llu version=%llu\n", (unsigned long long)entry.index, (unsigned long long)entry.version);
    fflush(stdout);
    funlockfile(stdout);
    c4_log_entry_free(&entry);
    for (index = 0U; index < sent_count; ++index)
    {
        Message response;
        message_init(&response);
        if (c4_rpc_transaction(cluster, sent_members[index], M_COMMIT, transaction_id, NULL, 0U, &response) < 0 || response.header.message_type != (uint8_t)M_ACK) prepare_ok = 0;
        message_free(&response);
    }
    if ((size_t)1U + sent_count != expected_replicas || !prepare_ok) errno = EHOSTUNREACH;
    free(sent_members);
    free(wire);
    free(operation);
    free(members);
    return prepare_ok ? 0 : -1;
submit_abort:
    (void)c4_log_abort(cluster->log, transaction_id);
    flockfile(stdout);
    printf("C5 ABORT transaction=");
    for (index = 0U; index < TRANSACTION_ID_SIZE; ++index) printf("%02x", transaction_id[index]);
    printf(" participants=%zu\n", sent_count);
    fflush(stdout);
    funlockfile(stdout);
    for (index = 0U; index < sent_count; ++index)
    {
        Message response;
        message_init(&response);
        (void)c4_rpc_transaction(cluster, sent_members[index], M_ABORT, transaction_id, NULL, 0U, &response);
        message_free(&response);
    }
submit_fail:
    free(sent_members);
    free(wire);
    free(operation);
    free(members);
    if (entry.payload == operation) entry.payload = NULL;
    if (entry.payload != NULL) c4_log_entry_free(&entry);
    return -1;
}

int c4_cluster_is_leader(C4Cluster *cluster)
{
    int result;
    if (cluster == NULL) return 0;
    (void)pthread_mutex_lock(&cluster->mutex);
    result = node_id_equal(&cluster->leader, &cluster->local.id);
    (void)pthread_mutex_unlock(&cluster->mutex);
    return result;
}

int c4_cluster_get_leader(C4Cluster *cluster, NodeID *leader)
{
    if (cluster == NULL || leader == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&cluster->mutex);
    *leader = cluster->leader;
    (void)pthread_mutex_unlock(&cluster->mutex);
    return c4_id_is_zero(leader->bytes) ? -1 : 0;
}

uint64_t c4_cluster_last_index(C4Cluster *cluster)
{
    return cluster == NULL ? 0U : c4_log_last_index(cluster->log);
}
