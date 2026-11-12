#define _POSIX_C_SOURCE 200809L
#include "heartbeat.h"
#include "gossip.h"
#include "rpc.h"
#include "network.h"
#include "wire.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define CONTROL_WORKERS 4U
#define CONTROL_QUEUE 64U
typedef struct { SuperPeerMember target; int gossip; } ControlJob;
struct Heartbeat
{
    Node local;
    SuperPeer *members;
    int owns_members;
    char ledger[600], epoch_path[600];
    uint64_t incarnation, sequence;
    NodeID home;
    MembershipFailure failure;
    PeerReconnect reconnect;
    void *context;
    unsigned interval, suspect, failed, removed, budget;
    size_t cursor, neighbor_cursor;
    pthread_mutex_t gate, queue_mutex;
    pthread_cond_t wake;
    pthread_t scheduler, workers[CONTROL_WORKERS];
    unsigned worker_count;
    int scheduler_started, stop;
    int needs_reconnect;
    ControlJob queue[CONTROL_QUEUE], active[CONTROL_WORKERS];
    int active_valid[CONTROL_WORKERS];
    size_t queued;
};

void heartbeat_gate_lock(Heartbeat *h) { if (h != NULL) pthread_mutex_lock(&h->gate); }
void heartbeat_gate_unlock(Heartbeat *h) { if (h != NULL) pthread_mutex_unlock(&h->gate); }

static int setting(const char *name, unsigned fallback, unsigned *output)
{
    const char *value = getenv(name);
    if (value == NULL) { *output = fallback; return 0; }
    char *end; errno = 0;
    unsigned long n = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || n < 100UL || n > 3600000UL) { errno = EINVAL; return -1; }
    *output = (unsigned)n;
    return 0;
}

/* Epoch gravada sob flock; rename e fsync do diretório tornam o incremento durável. */
static int advance_epoch(Heartbeat *h)
{
    char lock_path[620], temporary[620], parent[600];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", h->epoch_path) >= (int)sizeof(lock_path) || snprintf(temporary, sizeof(temporary), "%s.tmp", h->epoch_path) >= (int)sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    int lock = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (lock < 0) return -1;
    if (flock(lock, LOCK_EX) < 0) { close(lock); return -1; }
    uint8_t bytes[8]; uint64_t old = h->incarnation;
    int fd = open(h->epoch_path, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0)
    {
        uint8_t extra;
        if (read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes) || read(fd, &extra, 1U) != 0) { close(fd); close(lock); errno = EBADMSG; return -1; }
        close(fd);
        uint64_t disk = wire_get_u64(bytes);
        if (disk > old) old = disk;
    }
    else if (errno != ENOENT) { close(lock); return -1; }
    if (old == UINT64_MAX) { close(lock); errno = EOVERFLOW; return -1; }
    wire_put_u64(bytes, old + 1U);
    fd = open(temporary, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    int result = fd < 0 ? -1 : 0;
    if (result == 0 && (write(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes) || fsync(fd) < 0)) result = -1;
    if (fd >= 0 && close(fd) < 0) result = -1;
    if (result == 0) result = rename(temporary, h->epoch_path);
    strcpy(parent, h->epoch_path); char *slash = strrchr(parent, '/'); if (slash != NULL) *slash = '\0'; else strcpy(parent, ".");
    if (result == 0)
    {
        int directory = open(parent, O_RDONLY | O_DIRECTORY);
        if (directory < 0) result = -1;
        else { result = fsync(directory); close(directory); }
    }
    int error = errno; close(lock);
    if (result == 0) { h->incarnation = old + 1U; h->sequence = 0U; }
    else unlink(temporary);
    errno = error;
    return result;
}

uint64_t heartbeat_incarnation(Heartbeat *h)
{
    heartbeat_gate_lock(h); uint64_t result = h->incarnation; heartbeat_gate_unlock(h); return result;
}

unsigned heartbeat_budget(const Heartbeat *h) { return h == NULL ? 1000U : h->budget; }

int heartbeat_checkpoint(Heartbeat *h) { return membership_save(h->members, h->ledger); }

static int local_record(Heartbeat *h, SuperPeerMember *record)
{
    if (h->sequence == UINT64_MAX && advance_epoch(h) < 0) return -1;
    ++h->sequence;
    memset(record, 0, sizeof(*record));
    record->node = h->local; record->home = h->home; record->reporter = h->home;
    record->incarnation = h->incarnation; record->sequence = h->sequence;
    return membership_local(h->members, &h->local, h->incarnation, h->sequence);
}

static int encode_packet(Heartbeat *h, int gossip, uint8_t **output, uint32_t *size)
{
    SuperPeerMember self, *members = NULL; size_t count = 0U, n = 0U;
    if (local_record(h, &self) < 0) return -1;
    if (gossip && membership_snapshot(h->members, &members, &count) < 0) return -1;
    if (gossip)
    {
        size_t valid = 0U;
        for (size_t i = 0U; i < count; ++i) if (members[i].incarnation > 0U) members[valid++] = members[i];
        count = valid;
    }
    if (gossip) n = count < GOSSIP_BATCH_MAX ? count : GOSSIP_BATCH_MAX;
    *size = HEARTBEAT_WIRE_SIZE + (gossip ? 2U + (uint32_t)n * MEMBERSHIP_WIRE_SIZE : 0U);
    *output = malloc(*size);
    if (*output == NULL) { free(members); return -1; }
    (*output)[0] = 1U;
    int result = gossip_encode_record(&self, *output + 1U);
    if (gossip)
    {
        wire_put_u16(*output + HEARTBEAT_WIRE_SIZE, (uint16_t)n);
        for (size_t i = 0U; i < n && result == 0; ++i) result = gossip_encode_record(&members[(h->cursor + i) % count], *output + HEARTBEAT_WIRE_SIZE + 2U + i * MEMBERSHIP_WIRE_SIZE);
        if (count != 0U) h->cursor = (h->cursor + n) % count;
    }
    free(members);
    if (result < 0) { free(*output); *output = NULL; }
    return result;
}

/* Decodifica o lote inteiro antes de atualizar; o sender é evidência direta, as cópias não. */
static int apply_packet(Heartbeat *h, const uint8_t *payload, uint32_t size, const NodeID *source, int gossip)
{
    size_t n = 0U;
    if (payload == NULL || size < HEARTBEAT_WIRE_SIZE || payload[0] != 1U) { errno = EBADMSG; return -1; }
    if (gossip)
    {
        if (size < HEARTBEAT_WIRE_SIZE + 2U) { errno = EBADMSG; return -1; }
        n = wire_get_u16(payload + HEARTBEAT_WIRE_SIZE);
    }
    if (n > GOSSIP_BATCH_MAX || size != HEARTBEAT_WIRE_SIZE + (gossip ? 2U + n * MEMBERSHIP_WIRE_SIZE : 0U)) { errno = EBADMSG; return -1; }
    SuperPeerMember sender, batch[GOSSIP_BATCH_MAX];
    if (gossip_decode_record(payload + 1U, &sender) < 0 || !node_id_equal(source, &sender.node.id) || sender.state != SUPERPEER_MEMBER_ALIVE || sender.incarnation == 0U || sender.sequence == 0U || node_id_equal(source, &h->local.id) || (gossip && sender.node.role != NODE_ROLE_SUPERPEER)) { errno = EACCES; return -1; }
    if (sender.node.role == NODE_ROLE_SUPERPEER && !node_id_equal(&sender.home, source)) { errno = EACCES; return -1; }
    for (size_t i = 0U; i < n; ++i)
    {
        if (gossip_decode_record(payload + HEARTBEAT_WIRE_SIZE + 2U + i * MEMBERSHIP_WIRE_SIZE, &batch[i]) < 0 || batch[i].incarnation == 0U || (batch[i].node.role == NODE_ROLE_PEER && !node_id_equal(&batch[i].home, &batch[i].reporter))) { errno = EBADMSG; return -1; }
    }
    SuperPeerMember known;
    int known_result = superpeer_find_member(h->members, source, &known);
    if (h->local.role == NODE_ROLE_PEER && sender.node.role == NODE_ROLE_SUPERPEER && node_id_equal(source, &h->home) && (known_result < 0 || sender.incarnation > known.incarnation)) h->needs_reconnect = 1;
    if (known_result < 0)
    {
        /* Peers só podem entrar via JOIN; SPs têm descoberta própria, sem JOIN de Peer. */
        if (sender.node.role != NODE_ROLE_SUPERPEER) { errno = ENOENT; return -1; }
        SuperPeerMember *all; size_t count;
        if (membership_snapshot(h->members, &all, &count) < 0) return -1;
        int tombstone = 0;
        for (size_t i = 0U; i < count; ++i) if (node_id_equal(&all[i].node.id, source) && sender.incarnation <= all[i].incarnation && all[i].state >= SUPERPEER_MEMBER_FAILED) tombstone = 1;
        free(all);
        if (tombstone || membership_register(h->members, &sender.node, &sender.home, sender.incarnation, network_monotonic_ms()) == SUPERPEER_REGISTER_ERROR) { errno = ESTALE; return -1; }
    }
    if (membership_heartbeat(h->members, &sender, network_monotonic_ms()) < 0) return -1;
    for (size_t i = 0U; i < n; ++i)
    {
        if (node_id_equal(&batch[i].node.id, &h->local.id) && batch[i].state != SUPERPEER_MEMBER_ALIVE && batch[i].incarnation >= h->incarnation)
        {
            h->incarnation = batch[i].incarnation;
            if (advance_epoch(h) < 0) return -1;
        }
        else if (membership_merge(h->members, &batch[i]) < 0) return -1;
    }
    return heartbeat_checkpoint(h);
}

int heartbeat_handle(Heartbeat *h, const Message *request, Message_Type *type, uint8_t **payload, uint32_t *size)
{
    if (h == NULL || request == NULL || type == NULL || payload == NULL || size == NULL) { errno = EINVAL; return -1; }
    *payload = NULL; *size = 0U;
    if (memcmp(request->header.destination_node, h->local.id.bytes, NODE_ID_SIZE) != 0 || (request->header.message_type != M_HEARTBEAT && request->header.message_type != M_GOSSIP)) { errno = EACCES; return -1; }
    NodeID source; memcpy(source.bytes, request->header.source_node, NODE_ID_SIZE);
    int gossip = request->header.message_type == M_GOSSIP;
    if (gossip && h->local.role != NODE_ROLE_SUPERPEER) { errno = ENOTSUP; return -1; }
    heartbeat_gate_lock(h);
    int result = apply_packet(h, request->payload, request->header.payload_size, &source, gossip);
    if (result == 0) result = encode_packet(h, gossip, payload, size);
    heartbeat_gate_unlock(h);
    *type = gossip ? M_GOSSIP : M_HEARTBEAT;
    return result;
}

int heartbeat_register(Heartbeat *h, const Node *node, uint64_t incarnation)
{
    if (h == NULL || node == NULL) { errno = EINVAL; return -1; }
    heartbeat_gate_lock(h);
    int result = membership_register(h->members, node, &h->local.id, incarnation, network_monotonic_ms()) == SUPERPEER_REGISTER_ERROR ? -1 : 0;
    if (result == 0) result = heartbeat_checkpoint(h);
    heartbeat_gate_unlock(h);
    return result;
}

int heartbeat_discover(Heartbeat *h, const NodeConfig *config)
{
    Node node;
    if (h == NULL || config == NULL || node_init(&node, config) < 0) { errno = EINVAL; return -1; }
    node.role = NODE_ROLE_SUPERPEER;
    if (node_id_equal(&node.id, &h->local.id)) return 0;
    heartbeat_gate_lock(h);
    SuperPeerMember *all; size_t count;
    int result = membership_snapshot(h->members, &all, &count);
    if (result == 0)
    {
        int found = 0;
        for (size_t i = 0U; i < count; ++i) if (node_id_equal(&all[i].node.id, &node.id)) found = 1;
        free(all);
        if (!found) result = membership_register(h->members, &node, &node.id, 0U, network_monotonic_ms()) == SUPERPEER_REGISTER_ERROR ? -1 : 0;
    }
    heartbeat_gate_unlock(h);
    return result;
}

int heartbeat_peer_target(Heartbeat *h, const NodeConfig *config, PeerReconnect reconnect)
{
    if (heartbeat_discover(h, config) < 0) return -1;
    heartbeat_gate_lock(h);
    int result = node_compute_id(config, &h->home);
    h->reconnect = reconnect;
    heartbeat_gate_unlock(h);
    return result;
}

static void enqueue(Heartbeat *h, const SuperPeerMember *target, int gossip)
{
    pthread_mutex_lock(&h->queue_mutex);
    int duplicate = 0;
    for (size_t i = 0U; i < h->queued; ++i) if (node_id_equal(&h->queue[i].target.node.id, &target->node.id)) duplicate = 1;
    for (unsigned i = 0U; i < CONTROL_WORKERS; ++i) if (h->active_valid[i] == 1 && node_id_equal(&h->active[i].target.node.id, &target->node.id)) duplicate = 1;
    if (!h->stop && !duplicate && h->queued < CONTROL_QUEUE) { h->queue[h->queued++] = (ControlJob){*target, gossip}; pthread_cond_broadcast(&h->wake); }
    pthread_mutex_unlock(&h->queue_mutex);
}

static void execute_job(Heartbeat *h, const ControlJob *job)
{
    uint8_t *payload = NULL; uint32_t size;
    heartbeat_gate_lock(h); int result = encode_packet(h, job->gossip, &payload, &size); heartbeat_gate_unlock(h);
    Message response;
    if (result == 0) result = rpc_call_deadline(job->target.node.config.ip, job->target.node.config.port, &h->local.id, &job->target.node.id, job->gossip ? M_GOSSIP : M_HEARTBEAT, payload, size, &response, h->budget);
    free(payload);
    int rejected = 0;
    if (result == 0)
    {
        rejected = response.header.message_type == M_ERROR;
        heartbeat_gate_lock(h);
        result = response.header.message_type == (uint8_t)(job->gossip ? M_GOSSIP : M_HEARTBEAT) ? apply_packet(h, response.payload, response.header.payload_size, &job->target.node.id, job->gossip) : -1;
        if (rejected && advance_epoch(h) < 0) fprintf(stderr, "Nao foi possivel incrementar incarnation\n");
        heartbeat_gate_unlock(h);
        message_free(&response);
    }
    heartbeat_gate_lock(h);
    if (result < 0) (void)membership_probe(h->members, &job->target.node.id, job->target.incarnation, job->target.sequence, network_monotonic_ms(), 0);
    PeerReconnect reconnect = h->reconnect;
    int needs_reconnect = h->needs_reconnect;
    heartbeat_gate_unlock(h);
    /* Nenhum mutex permanece adquirido enquanto JOIN/ANNOUNCE fazem TCP. */
    if ((result < 0 || needs_reconnect) && reconnect != NULL && h->local.role == NODE_ROLE_PEER && !job->gossip)
    {
        if (result < 0 && !rejected) { heartbeat_gate_lock(h); result = advance_epoch(h); heartbeat_gate_unlock(h); }
        if (rejected || result == 0)
        {
            if (reconnect(h->context) < 0) { heartbeat_gate_lock(h); h->needs_reconnect = 1; heartbeat_gate_unlock(h); fprintf(stderr, "Peer aguardando reconexao ao Super Peer\n"); }
            else { heartbeat_gate_lock(h); h->needs_reconnect = 0; heartbeat_gate_unlock(h); }
        }
    }
}

static void *worker(void *context)
{
    Heartbeat *h = context;
    pthread_mutex_lock(&h->queue_mutex);
    unsigned slot;
    for (slot = 0U; slot < CONTROL_WORKERS; ++slot) if (!h->active_valid[slot]) { h->active_valid[slot] = 2; break; }
    for (;;)
    {
        while (!h->stop && h->queued == 0U) pthread_cond_wait(&h->wake, &h->queue_mutex);
        if (h->stop) break;
        ControlJob job = h->queue[0];
        memmove(h->queue, h->queue + 1U, (--h->queued) * sizeof(*h->queue));
        h->active[slot] = job; h->active_valid[slot] = 1;
        pthread_mutex_unlock(&h->queue_mutex);
        execute_job(h, &job);
        pthread_mutex_lock(&h->queue_mutex); h->active_valid[slot] = 2;
    }
    h->active_valid[slot] = 0;
    pthread_mutex_unlock(&h->queue_mutex);
    return NULL;
}

static void evaluate(Heartbeat *h)
{
    SuperPeerMember *before = NULL, *after = NULL; size_t old_count = 0U, count = 0U;
    heartbeat_gate_lock(h);
    if (membership_snapshot(h->members, &before, &old_count) == 0 && membership_tick(h->members, network_monotonic_ms(), h->suspect, h->failed, h->removed) == 0 && membership_snapshot(h->members, &after, &count) == 0)
    {
        for (size_t i = 0U; i < count; ++i)
        {
            int newly_failed = after[i].state >= SUPERPEER_MEMBER_FAILED;
            for (size_t j = 0U; j < old_count; ++j) if (node_id_equal(&before[j].node.id, &after[i].node.id) && before[j].state >= SUPERPEER_MEMBER_FAILED) newly_failed = 0;
            if (newly_failed && h->failure != NULL) h->failure(h->context, &after[i]);
        }
        if (heartbeat_checkpoint(h) < 0) perror("membership persistence");
    }
    heartbeat_gate_unlock(h);
    free(before); free(after);
}

static void *schedule(void *context)
{
    Heartbeat *h = context; int64_t next = 0;
    for (;;)
    {
        pthread_mutex_lock(&h->queue_mutex); int stop = h->stop; pthread_mutex_unlock(&h->queue_mutex);
        if (stop) break;
        evaluate(h);
        int64_t now = network_monotonic_ms(); int periodic = now >= next;
        SuperPeerMember *members; size_t count;
        if (membership_snapshot(h->members, &members, &count) == 0)
        {
            unsigned fanout = 0U;
            for (size_t offset = 0U; offset < count; ++offset)
            {
                size_t i = (h->neighbor_cursor + offset) % count;
                SuperPeerMember *m = &members[i];
                if (node_id_equal(&m->node.id, &h->local.id)) continue;
                if (h->local.role == NODE_ROLE_PEER)
                {
                    heartbeat_gate_lock(h); int target = node_id_equal(&m->node.id, &h->home); heartbeat_gate_unlock(h);
                    if (target && periodic) enqueue(h, m, 0);
                }
                else
                {
                    if (m->local_registration && m->state == SUPERPEER_MEMBER_SUSPECT && now - m->probe_ms >= 1000) enqueue(h, m, 0);
                    else if (periodic && m->node.role == NODE_ROLE_SUPERPEER && m->state < SUPERPEER_MEMBER_FAILED)
                    {
                        int gossip = fanout < 3U;
                        enqueue(h, m, gossip);
                        if (gossip) ++fanout;
                    }
                    /* Tombstones de SP são sondados: uma época nova pode voltar ao anel. */
                    if (periodic && m->node.role == NODE_ROLE_SUPERPEER && m->state >= SUPERPEER_MEMBER_FAILED) enqueue(h, m, 0);
                }
            }
            if (count != 0U && periodic) h->neighbor_cursor = (h->neighbor_cursor + 3U) % count;
            free(members);
        }
        if (periodic) next = now + h->interval;
        pthread_mutex_lock(&h->queue_mutex);
        struct timespec deadline; clock_gettime(CLOCK_MONOTONIC, &deadline); deadline.tv_nsec += 100000000L;
        if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
        if (!h->stop) (void)pthread_cond_timedwait(&h->wake, &h->queue_mutex, &deadline);
        pthread_mutex_unlock(&h->queue_mutex);
    }
    return NULL;
}

int heartbeat_create(const Node *local, SuperPeer *members, const char *directory, MembershipFailure failure, void *context, Heartbeat **output)
{
    if (local == NULL || directory == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = NULL;
    Heartbeat *h = calloc(1U, sizeof(*h)); if (h == NULL) return -1;
    h->local = *local; h->members = members; h->failure = failure; h->context = context; h->home = local->id;
    if (snprintf(h->ledger, sizeof(h->ledger), "%s/membership.bin", directory) >= (int)sizeof(h->ledger) || snprintf(h->epoch_path, sizeof(h->epoch_path), "%s/node.incarnation", directory) >= (int)sizeof(h->epoch_path) || setting("C3_HEARTBEAT_MS", 5000U, &h->interval) < 0 || setting("C3_SUSPECT_MS", 15000U, &h->suspect) < 0 || setting("C3_FAILED_MS", 20000U, &h->failed) < 0 || setting("C3_REMOVED_MS", 30000U, &h->removed) < 0 || setting("C3_RPC_MS", 1000U, &h->budget) < 0 || h->interval >= h->suspect || h->suspect >= h->failed || h->failed >= h->removed || h->budget >= h->suspect) { free(h); errno = EINVAL; return -1; }
    if (members == NULL)
    {
        SuperPeerConfig config = {.node = local->config};
        if (superpeer_create(&config, &h->members) < 0) { free(h); return -1; }
        h->owns_members = 1;
    }
    int error = pthread_mutex_init(&h->gate, NULL);
    if (error != 0) { if (h->owns_members) superpeer_destroy(h->members); free(h); errno = error; return -1; }
    error = pthread_mutex_init(&h->queue_mutex, NULL);
    if (error != 0) { pthread_mutex_destroy(&h->gate); if (h->owns_members) superpeer_destroy(h->members); free(h); errno = error; return -1; }
    pthread_condattr_t attr; pthread_condattr_init(&attr); pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    error = pthread_cond_init(&h->wake, &attr); pthread_condattr_destroy(&attr);
    if (error != 0) { pthread_mutex_destroy(&h->queue_mutex); pthread_mutex_destroy(&h->gate); if (h->owns_members) superpeer_destroy(h->members); free(h); errno = error; return -1; }
    if (advance_epoch(h) < 0 || membership_load(h->members, h->ledger) < 0 || membership_local(h->members, local, h->incarnation, 0U) < 0) { heartbeat_destroy(h); return -1; }
    *output = h; return 0;
}

int heartbeat_start(Heartbeat *h)
{
    if (h == NULL) { errno = EINVAL; return -1; }
    for (unsigned i = 0U; i < CONTROL_WORKERS; ++i)
    {
        int error = pthread_create(&h->workers[i], NULL, worker, h);
        if (error != 0) { errno = error; return -1; }
        ++h->worker_count;
    }
    int error = pthread_create(&h->scheduler, NULL, schedule, h);
    if (error != 0) { errno = error; return -1; }
    h->scheduler_started = 1; return 0;
}

void heartbeat_stop(Heartbeat *h)
{
    if (h == NULL) return;
    pthread_mutex_lock(&h->queue_mutex); h->stop = 1; h->queued = 0U; pthread_cond_broadcast(&h->wake); pthread_mutex_unlock(&h->queue_mutex);
    if (h->scheduler_started) { pthread_join(h->scheduler, NULL); h->scheduler_started = 0; }
    for (unsigned i = 0U; i < h->worker_count; ++i) pthread_join(h->workers[i], NULL);
    h->worker_count = 0U;
}

void heartbeat_destroy(Heartbeat *h)
{
    if (h == NULL) return;
    heartbeat_stop(h);
    pthread_cond_destroy(&h->wake); pthread_mutex_destroy(&h->queue_mutex); pthread_mutex_destroy(&h->gate);
    if (h->owns_members) superpeer_destroy(h->members);
    free(h);
}
