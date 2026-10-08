#define _POSIX_C_SOURCE 200809L

#include "superpeer.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "gossip.h"
#include "wire.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>


struct SuperPeer
{
    Node local_node;
    SuperPeerMember *members;
    size_t member_count;
    size_t member_capacity;
    pthread_mutex_t members_mutex;
};

/* Adquire mutex; converte erro de pthread para -1/errno. */
static int lock_members(SuperPeer *superpeer)
{
    int error = pthread_mutex_lock(&superpeer->members_mutex);

    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return 0;
}

/* Libera mutex e propaga eventual erro por errno. */
static int unlock_members(SuperPeer *superpeer)
{
    int error = pthread_mutex_unlock(&superpeer->members_mutex);

    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return 0;
}

/* Busca linear O(N); exige que o chamador já tenha adquirido o mutex. */
static int find_member_index_locked(const SuperPeer *superpeer, const NodeID *node_id, size_t *index)
{
    size_t i;
    // O index é opcional, então não precisamos inicializá-lo aqui.
    for (i = 0U; i < superpeer->member_count; ++i)
    {
        if (node_id_equal(&superpeer->members[i].node.id, node_id))
        {
            if (index != NULL)
            {
                *index = i;
            }
            return 0;
        }
    }
    return -1;
}

/* Sob mutex, duplica capacidade com verificação de overflow; preserva ponteiro se realloc falhar. */
static int grow_members_locked(SuperPeer *superpeer)
{
    if (superpeer->member_count >= 65536U) { errno = ENOSPC; return -1; }
    size_t new_capacity;
    SuperPeerMember *new_members;

    // Se ainda há espaço, não é necessário crescer.
    if (superpeer->member_count < superpeer->member_capacity)
    {
        return 0;
    }

    // Se a capacidade atual for zero, inicializamos com a capacidade padrão. Caso contrário, dobramos a capacidade.
    if (superpeer->member_capacity == 0U)
    {
        new_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    }
    else
    {
        if (superpeer->member_capacity > SIZE_MAX / 2U)
        {
            errno = ENOMEM;
            return -1;
        }
        new_capacity = superpeer->member_capacity * 2U;
    }

    // Verifica se a nova capacidade multiplicada pelo tamanho do elemento não causa overflow.
    if (new_capacity > SIZE_MAX / sizeof(*new_members))
    {
        errno = ENOMEM;
        return -1;
    }

    // Realoca a memória para os membros com a nova capacidade.
    new_members = realloc(superpeer->members, new_capacity * sizeof(*new_members));
    if (new_members == NULL)
    {
        errno = ENOMEM;
        return -1;
    }

    superpeer->members = new_members;
    superpeer->member_capacity = new_capacity;
    return 0;
}

/* Inicializa registro; somente a API C3 aplica heartbeat com sequência validada. */
static void set_member(SuperPeerMember *member, const Node *node)
{
    memset(member, 0, sizeof(*member));
    member->node = *node;
    member->state = SUPERPEER_MEMBER_ALIVE;
    member->last_seen = time(NULL);
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    member->observed_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    member->local_registration = 1;
}

/* Configura identidade conhecida e capacidade inicial padrão de 16 membros. */
int superpeer_config_init_with_uuid(SuperPeerConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE])
{
    // Valida parâmetros de entrada.
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    // Configura o nó com o UUID fornecido.
    if (node_config_init_with_uuid(&config->node, ip, port, uuid) == -1)
    {
        return -1;
    }

    config->initial_member_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    return 0;
}

/* Configura identidade com UUID aleatório e capacidade padrão 16. */
int superpeer_config_init(SuperPeerConfig *config, const char *ip, uint16_t port)
{
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_init(&config->node, ip, port) == -1)
    {
        return -1;
    }

    config->initial_member_capacity = SUPERPEER_DEFAULT_MEMBER_CAPACITY;
    return 0;
}

/* Aloca tabela/mutex e autorregistra o Super Peer: contagem inicial 1. Desfaz alocações em falha. */
int superpeer_create(const SuperPeerConfig *config, SuperPeer **output)
{
    // Valida parâmetros de entrada e inicializa o nó local.
    SuperPeer *superpeer;
    size_t initial_capacity;
    int mutex_error;
    Node local_node;

    if (output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    *output = NULL;
    if (config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_validate(&config->node) == -1 || node_init(&local_node, &config->node) == -1)
    {
        return -1;
    }
    initial_capacity = config->initial_member_capacity == 0U ? SUPERPEER_DEFAULT_MEMBER_CAPACITY : config->initial_member_capacity;

    if (initial_capacity > SIZE_MAX / sizeof(SuperPeerMember))
    {
        errno = ENOMEM;
        return -1;
    }

    superpeer = calloc(1U, sizeof(*superpeer));
    if (superpeer == NULL)
    {
        return -1;
    }

    superpeer->members = calloc(initial_capacity, sizeof(*superpeer->members));
    if (superpeer->members == NULL)
    {
        free(superpeer);
        return -1;
    }

    mutex_error = pthread_mutex_init(&superpeer->members_mutex, NULL);
    if (mutex_error != 0)
    {
        free(superpeer->members);
        free(superpeer);
        errno = mutex_error;
        return -1;
    }

    // Inicializa o nó local com papel de Super Peer e registra-o como o primeiro membro.
    local_node.role = NODE_ROLE_SUPERPEER;
    superpeer->local_node = local_node;
    superpeer->member_capacity = initial_capacity;
    set_member(&superpeer->members[0], &superpeer->local_node);
    superpeer->member_count = 1U;
    *output = superpeer;
    return 0;
}

/* Libera recursos; o chamador deve encerrar threads usuárias antes de destruir. */
void superpeer_destroy(SuperPeer *superpeer)
{
    if (superpeer == NULL)
    {
        return;
    }

    pthread_mutex_destroy(&superpeer->members_mutex);
    free(superpeer->members);
    free(superpeer);
}

/* Copia o nó local, imutável após criação. */
int superpeer_get_node(const SuperPeer *superpeer, Node *output)
{
    if (superpeer == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    *output = superpeer->local_node;
    return 0;
}

/* Valida nó; sob mutex, adiciona ou atualiza por ID. Duplicata não aumenta contagem. */
SuperPeerRegistrationResult superpeer_register_node(SuperPeer *superpeer, const Node *node)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return membership_register(superpeer, node, superpeer == NULL ? NULL : &superpeer->local_node.id, 0U, (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/* LEAVE lógico: protege nó local e mantém tombstone para bloquear mensagens atrasadas. */
int superpeer_unregister_node(SuperPeer *superpeer, const NodeID *node_id)
{
    size_t index;

    if (superpeer == NULL || node_id == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_id_equal(&superpeer->local_node.id, node_id))
    {
        errno = EPERM;
        return -1;
    }
    if (lock_members(superpeer) == -1)
    {
        return -1;
    }

    if (find_member_index_locked(superpeer, node_id, &index) == -1)
    {
        unlock_members(superpeer);
        errno = ENOENT;
        return -1;
    }

    superpeer->members[index].state = SUPERPEER_MEMBER_REMOVED;
    superpeer->members[index].reporter = superpeer->local_node.id;
    ++superpeer->members[index].version;

    if (unlock_members(superpeer) == -1)
    {
        return -1;
    }
    return 0;
}

/* Busca sob mutex e retorna cópia: nenhum ponteiro interno sobrevive a realloc. */
int superpeer_find_member(const SuperPeer *superpeer, const NodeID *node_id, SuperPeerMember *output)
{
    SuperPeer *mutable_superpeer;
    size_t index;

    if (superpeer == NULL || node_id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return -1;
    }

    if (find_member_index_locked(superpeer, node_id, &index) == -1 || superpeer->members[index].state == SUPERPEER_MEMBER_REMOVED)
    {
        unlock_members(mutable_superpeer);
        errno = ENOENT;
        return -1;
    }

    *output = superpeer->members[index];
    if (unlock_members(mutable_superpeer) == -1)
    {
        return -1;
    }
    return 0;
}

/* Consulta contagem sob mutex, incluindo o nó local; zero também pode sinalizar erro. */
size_t superpeer_member_count(const SuperPeer *superpeer)
{
    SuperPeer *mutable_superpeer;
    size_t count;

    if (superpeer == NULL)
    {
        errno = EINVAL;
        return 0U;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return 0U;
    }
    count = 0U;
    for (size_t i = 0U; i < superpeer->member_count; ++i) if (superpeer->members[i].local_registration && superpeer->members[i].state < SUPERPEER_MEMBER_FAILED) ++count;
    unlock_members(mutable_superpeer);
    return count;
}

/* Consulta existência sob mutex; zero significa ausência ou erro. */
int superpeer_is_registered(const SuperPeer *superpeer, const NodeID *node_id)
{
    SuperPeer *mutable_superpeer;
    int registered;

    if (superpeer == NULL || node_id == NULL)
    {
        errno = EINVAL;
        return 0;
    }

    mutable_superpeer = (SuperPeer *)(void *)superpeer;
    if (lock_members(mutable_superpeer) == -1)
    {
        return 0;
    }
    size_t index;
    registered = find_member_index_locked(superpeer, node_id, &index) == 0 && superpeer->members[index].local_registration && superpeer->members[index].state < SUPERPEER_MEMBER_FAILED;
    unlock_members(mutable_superpeer);
    return registered;
}

const char *membership_state_name(SuperPeerMemberState state)
{
    static const char *names[] = {"ALIVE", "SUSPECT", "FAILED", "REMOVED"};
    return state <= SUPERPEER_MEMBER_REMOVED ? names[state] : "INVALID";
}

static void transition(SuperPeer *table, SuperPeerMember *member, SuperPeerMemberState state)
{
    if (member->state == state) return;
    char id[NODE_ID_HEX_SIZE];
    (void)node_id_to_hex(&member->node.id, id, sizeof(id));
    printf("Membership %s %s -> %s role=%s incarnation=%llu\n", id, membership_state_name(member->state), membership_state_name(state), member->node.role == NODE_ROLE_PEER ? "Peer" : "SP", (unsigned long long)member->incarnation);
    fflush(stdout);
    member->state = state;
    member->reporter = table->local_node.id;
    ++member->version;
}

SuperPeerRegistrationResult membership_register(SuperPeer *table, const Node *node, const NodeID *home, uint64_t incarnation, int64_t now)
{
    if (table == NULL || node == NULL || home == NULL || node_validate(node) < 0) { errno = EINVAL; return SUPERPEER_REGISTER_ERROR; }
    if (lock_members(table) < 0) return SUPERPEER_REGISTER_ERROR;
    size_t index;
    int exists = find_member_index_locked(table, &node->id, &index) == 0;
    if (exists && (incarnation < table->members[index].incarnation || (table->members[index].state >= SUPERPEER_MEMBER_FAILED && incarnation <= table->members[index].incarnation))) { unlock_members(table); errno = ESTALE; return SUPERPEER_REGISTER_ERROR; }
    if (!exists)
    {
        if (grow_members_locked(table) < 0) { unlock_members(table); return SUPERPEER_REGISTER_ERROR; }
        index = table->member_count++;
        set_member(&table->members[index], node);
    }
    SuperPeerMember *member = &table->members[index];
    uint64_t sequence = incarnation == member->incarnation ? member->sequence : 0U;
    member->node = *node;
    member->home = *home;
    member->incarnation = incarnation;
    member->sequence = sequence;
    transition(table, member, SUPERPEER_MEMBER_ALIVE);
    member->reporter = table->local_node.id;
    member->observed_ms = now;
    member->last_seen = time(NULL);
    member->failed_probes = 0U;
    member->local_registration = node->role == NODE_ROLE_SUPERPEER || node_id_equal(home, &table->local_node.id);
    unlock_members(table);
    return exists ? SUPERPEER_MEMBER_UPDATED : SUPERPEER_MEMBER_ADDED;
}

int membership_heartbeat(SuperPeer *table, const SuperPeerMember *incoming, int64_t now)
{
    if (table == NULL || incoming == NULL || node_validate(&incoming->node) < 0 || incoming->state != SUPERPEER_MEMBER_ALIVE || incoming->incarnation == 0U) { errno = EINVAL; return -1; }
    if (lock_members(table) < 0) return -1;
    size_t index;
    if (find_member_index_locked(table, &incoming->node.id, &index) < 0) { unlock_members(table); errno = ENOENT; return -1; }
    SuperPeerMember *member = &table->members[index];
    if (incoming->node.role != member->node.role || !node_id_equal(&incoming->home, &member->home) || incoming->incarnation < member->incarnation || (incoming->incarnation == member->incarnation && (incoming->sequence <= member->sequence || member->state >= SUPERPEER_MEMBER_FAILED))) { unlock_members(table); errno = ESTALE; return -1; }
    member->incarnation = incoming->incarnation;
    member->sequence = incoming->sequence;
    transition(table, member, SUPERPEER_MEMBER_ALIVE);
    member->observed_ms = now;
    member->last_seen = time(NULL);
    member->failed_probes = 0U;
    unlock_members(table);
    return 0;
}

int membership_snapshot(SuperPeer *table, SuperPeerMember **output, size_t *count)
{
    if (table == NULL || output == NULL || count == NULL) { errno = EINVAL; return -1; }
    *output = NULL; *count = 0U;
    if (lock_members(table) < 0) return -1;
    SuperPeerMember *copy = malloc(table->member_count * sizeof(*copy));
    if (copy == NULL) { unlock_members(table); return -1; }
    memcpy(copy, table->members, table->member_count * sizeof(*copy));
    *output = copy; *count = table->member_count;
    unlock_members(table);
    return 0;
}

/* Não renova o relógio direto: uma cópia ALIVE antiga não prova presença do nó. */
int membership_merge(SuperPeer *table, const SuperPeerMember *incoming)
{
    if (table == NULL || incoming == NULL || node_validate(&incoming->node) < 0 || incoming->state > SUPERPEER_MEMBER_REMOVED) { errno = EINVAL; return -1; }
    if (node_id_equal(&incoming->node.id, &table->local_node.id)) return 0;
    if (incoming->node.role == NODE_ROLE_PEER && !node_id_equal(&incoming->home, &incoming->reporter)) { errno = EACCES; return -1; }
    if (lock_members(table) < 0) return -1;
    size_t index;
    if (find_member_index_locked(table, &incoming->node.id, &index) < 0)
    {
        if (grow_members_locked(table) < 0) { unlock_members(table); return -1; }
        index = table->member_count++;
        table->members[index] = *incoming;
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        table->members[index].observed_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        table->members[index].local_registration = incoming->node.role == NODE_ROLE_SUPERPEER;
    }
    else
    {
        SuperPeerMember *old = &table->members[index];
        int newer = incoming->incarnation > old->incarnation || (incoming->incarnation == old->incarnation && (incoming->sequence > old->sequence || (incoming->sequence == old->sequence && (incoming->state > old->state || (incoming->state == old->state && (incoming->version > old->version || (incoming->version == old->version && node_id_compare(&incoming->reporter, &old->reporter) > 0)))))));
        if (old->state == SUPERPEER_MEMBER_REMOVED && incoming->incarnation <= old->incarnation) newer = 0;
        /* O domínio local não aceita que outro observador altere seus Peers. */
        if (old->node.role == NODE_ROLE_PEER && node_id_equal(&old->home, &table->local_node.id)) newer = 0;
        if (newer)
        {
            int64_t observed = old->observed_ms;
            /* ALIVE por rumor após suspeita/falha não comprova a recuperação para o Chord. */
            time_t last_seen = incoming->incarnation == old->incarnation && (old->state == SUPERPEER_MEMBER_ALIVE || incoming->state != SUPERPEER_MEMBER_ALIVE) ? old->last_seen : 0;
            int local = old->local_registration;
            transition(table, old, incoming->state);
            *old = *incoming;
            old->observed_ms = observed;
            old->last_seen = last_seen;
            old->local_registration = local;
        }
    }
    unlock_members(table);
    return 0;
}

int membership_probe(SuperPeer *table, const NodeID *id, uint64_t incarnation, uint64_t sequence, int64_t now, int success)
{
    if (table == NULL || id == NULL) { errno = EINVAL; return -1; }
    if (lock_members(table) < 0) return -1;
    size_t index;
    if (find_member_index_locked(table, id, &index) == 0)
    {
        SuperPeerMember *m = &table->members[index];
        if (m->incarnation == incarnation && m->sequence == sequence && m->state == SUPERPEER_MEMBER_SUSPECT && now - m->probe_ms >= 1000)
        {
            m->probe_ms = now;
            if (!success && m->failed_probes < 2U) ++m->failed_probes;
            if (success) m->failed_probes = 0U;
        }
    }
    unlock_members(table);
    return 0;
}

int membership_tick(SuperPeer *table, int64_t now, unsigned suspect_ms, unsigned failed_ms, unsigned removed_ms)
{
    if (table == NULL || suspect_ms == 0U || suspect_ms >= failed_ms || failed_ms >= removed_ms) { errno = EINVAL; return -1; }
    if (lock_members(table) < 0) return -1;
    for (size_t i = 0U; i < table->member_count; ++i)
    {
        SuperPeerMember *m = &table->members[i];
        if (node_id_equal(&m->node.id, &table->local_node.id) || !m->local_registration) continue;
        int64_t age = now - m->observed_ms;
        if (m->state == SUPERPEER_MEMBER_ALIVE && age >= suspect_ms) transition(table, m, SUPERPEER_MEMBER_SUSPECT);
        if (m->state == SUPERPEER_MEMBER_SUSPECT && age >= failed_ms && m->failed_probes >= 2U) transition(table, m, SUPERPEER_MEMBER_FAILED);
        if (m->state == SUPERPEER_MEMBER_FAILED && age >= removed_ms) transition(table, m, SUPERPEER_MEMBER_REMOVED);
    }
    unlock_members(table);
    return 0;
}

int membership_local(SuperPeer *table, const Node *node, uint64_t incarnation, uint64_t sequence)
{
    if (table == NULL || node == NULL || !node_id_equal(&table->local_node.id, &node->id)) { errno = EINVAL; return -1; }
    if (lock_members(table) < 0) return -1;
    table->local_node = *node;
    SuperPeerMember *m = &table->members[0];
    m->node = *node; m->home = node->id; m->reporter = node->id;
    m->incarnation = incarnation; m->sequence = sequence; m->state = SUPERPEER_MEMBER_ALIVE;
    unlock_members(table);
    return 0;
}

static int exact_file(int fd, uint8_t *data, size_t size, int writing)
{
    size_t done = 0U;
    while (done < size)
    {
        ssize_t n = writing ? write(fd, data + done, size - done) : read(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EBADMSG; return -1; }
        done += (size_t)n;
    }
    return 0;
}

/* Publicação atômica do ledger; não contém bytes de PDFs nem relógios monotônicos. */
int membership_save(SuperPeer *table, const char *path)
{
    if (table == NULL || path == NULL) { errno = EINVAL; return -1; }
    SuperPeerMember *members; size_t count;
    if (membership_snapshot(table, &members, &count) < 0) return -1;
    char temporary[PATH_MAX], parent[PATH_MAX];
    if (count > 65536U || snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary) || strlen(path) >= sizeof(parent)) { free(members); errno = EOVERFLOW; return -1; }
    int fd = open(temporary, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    if (fd < 0) { free(members); return -1; }
    uint8_t header[12] = {'P','D','M','E','M','B','1',0};
    wire_put_u32(header + 8U, (uint32_t)count);
    int result = exact_file(fd, header, sizeof(header), 1);
    for (size_t i = 0U; i < count && result == 0; ++i)
    {
        uint8_t record[MEMBERSHIP_WIRE_SIZE];
        result = gossip_encode_record(&members[i], record);
        if (result == 0) result = exact_file(fd, record, sizeof(record), 1);
    }
    free(members);
    if (result == 0) result = fsync(fd);
    int error = errno;
    if (close(fd) < 0 && result == 0) { result = -1; error = errno; }
    if (result == 0) result = rename(temporary, path);
    if (result == 0)
    {
        strcpy(parent, path);
        char *slash = strrchr(parent, '/');
        if (slash == NULL) strcpy(parent, "."); else *slash = '\0';
        int directory = open(parent, O_RDONLY | O_DIRECTORY);
        if (directory < 0) result = -1;
        else { result = fsync(directory); close(directory); }
    }
    if (result < 0) { if (errno == 0) errno = error; unlink(temporary); }
    return result;
}

int membership_load(SuperPeer *table, const char *path)
{
    if (table == NULL || path == NULL) { errno = EINVAL; return -1; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    uint8_t header[12];
    int result = exact_file(fd, header, sizeof(header), 0);
    if (result < 0 || memcmp(header, "PDMEMB1", 8U) != 0 || wire_get_u32(header + 8U) > 65536U) { close(fd); errno = EBADMSG; return -1; }
    size_t count = wire_get_u32(header + 8U);
    SuperPeerMember *members = calloc(count == 0U ? 1U : count, sizeof(*members));
    if (members == NULL) { close(fd); return -1; }
    for (size_t i = 0U; i < count && result == 0; ++i)
    {
        uint8_t record[MEMBERSHIP_WIRE_SIZE];
        result = exact_file(fd, record, sizeof(record), 0);
        if (result == 0) result = gossip_decode_record(record, &members[i]);
    }
    uint8_t extra;
    if (result == 0 && read(fd, &extra, 1U) != 0) { result = -1; errno = EBADMSG; }
    close(fd);
    if (result == 0)
    {
        struct timespec now; (void)clock_gettime(CLOCK_MONOTONIC, &now);
        lock_members(table);
        for (size_t i = 0U; i < count; ++i)
        {
            if (node_id_equal(&members[i].node.id, &table->local_node.id)) continue;
            if (grow_members_locked(table) < 0) { result = -1; break; }
            members[i].local_registration = members[i].node.role == NODE_ROLE_SUPERPEER || node_id_equal(&members[i].home, &table->local_node.id);
            members[i].observed_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            table->members[table->member_count++] = members[i];
        }
        unlock_members(table);
    }
    free(members);
    return result;
}
