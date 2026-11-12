#define _POSIX_C_SOURCE 200809L
#include "chord.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct UnavailableNode
{
    NodeID id;
    struct UnavailableNode *next;
} UnavailableNode;

struct Chord
{
    ChordPeer local;
    ChordPeer successor;
    ChordPeer predecessor;
    ChordPeer fingers[CHORD_FINGER_COUNT];
    int has_predecessor;
    UnavailableNode *unavailable;
    pthread_mutex_t mutex;
};

/* Soma 2^bit no identificador de 256 bits em ordem big-endian. */
void chord_id_add_power(const NodeID *base, unsigned bit, NodeID *result)
{
    unsigned byte_index;
    unsigned carry;
    if (base == NULL || result == NULL || bit >= CHORD_FINGER_COUNT) return;
    *result = *base;
    byte_index = 31U - bit / 8U;
    carry = 1U << (bit % 8U);
    for (;;)
    {
        unsigned sum = (unsigned)result->bytes[byte_index] + carry;
        result->bytes[byte_index] = (uint8_t)sum;
        carry = sum >> 8U;
        if (carry == 0U || byte_index == 0U) break;
        --byte_index;
    }
}

/* O intervalo circular (start, end] inclui todo o anel quando ambos coincidem. */
int chord_interval_open_closed(const NodeID *start, const NodeID *end, const NodeID *value)
{
    int order;
    if (start == NULL || end == NULL || value == NULL) return 0;
    order = node_id_compare(start, end);
    if (order == 0) return 1;
    if (order < 0) return node_id_compare(start, value) < 0 && node_id_compare(value, end) <= 0;
    return node_id_compare(start, value) < 0 || node_id_compare(value, end) <= 0;
}

/* O intervalo circular aberto nunca inclui seus extremos. */
int chord_interval_open_open(const NodeID *start, const NodeID *end, const NodeID *value)
{
    int order;
    if (start == NULL || end == NULL || value == NULL) return 0;
    order = node_id_compare(start, end);
    if (order == 0) return !node_id_equal(start, value);
    if (order < 0) return node_id_compare(start, value) < 0 && node_id_compare(value, end) < 0;
    return node_id_compare(start, value) < 0 || node_id_compare(value, end) < 0;
}

/* Inicializa o anel isolado: sucessor e todas as entradas apontam para si. */
int chord_create(const Node *local, Chord **output)
{
    Chord *chord;
    int error;
    if (local == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = NULL;
    chord = calloc(1U, sizeof(*chord));
    if (chord == NULL) return -1;
    chord->local.id = local->id;
    chord->local.config = local->config;
    chord->successor = chord->local;
    for (unsigned i = 0U; i < CHORD_FINGER_COUNT; ++i) chord->fingers[i] = chord->local;
    error = pthread_mutex_init(&chord->mutex, NULL);
    if (error != 0) { free(chord); errno = error; return -1; }
    *output = chord;
    return 0;
}

void chord_destroy(Chord *chord)
{
    if (chord == NULL) return;
    while (chord->unavailable != NULL)
    {
        UnavailableNode *next = chord->unavailable->next;
        free(chord->unavailable);
        chord->unavailable = next;
    }
    pthread_mutex_destroy(&chord->mutex);
    free(chord);
}

static int valid_peer(const ChordPeer *peer)
{
    NodeID calculated;
    return peer != NULL && node_compute_id(&peer->config, &calculated) == 0 && node_id_equal(&calculated, &peer->id);
}

/* Exige mutex adquirido. Impede reaplicar uma resposta TCP anterior à confirmação de falha. */
static int unavailable_locked(const Chord *chord, const NodeID *id)
{
    for (const UnavailableNode *entry = chord->unavailable; entry != NULL; entry = entry->next) if (node_id_equal(&entry->id, id)) return 1;
    return 0;
}

int chord_local(Chord *chord, ChordPeer *output)
{
    if (chord == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = chord->local;
    return 0;
}

int chord_get_successor(Chord *chord, ChordPeer *output)
{
    if (chord == NULL || output == NULL) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    *output = chord->successor;
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

int chord_get_predecessor(Chord *chord, ChordPeer *output, int *exists)
{
    if (chord == NULL || output == NULL || exists == NULL) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    *exists = chord->has_predecessor;
    if (*exists) *output = chord->predecessor;
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

int chord_get_finger(Chord *chord, unsigned index, ChordPeer *output)
{
    if (chord == NULL || output == NULL || index >= CHORD_FINGER_COUNT) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    *output = chord->fingers[index];
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

int chord_set_successor(Chord *chord, const ChordPeer *peer)
{
    if (chord == NULL || !valid_peer(peer)) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    if (unavailable_locked(chord, &peer->id)) { pthread_mutex_unlock(&chord->mutex); errno = EHOSTUNREACH; return -1; }
    chord->successor = *peer;
    chord->fingers[0] = *peer;
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

int chord_notify(Chord *chord, const ChordPeer *peer)
{
    if (chord == NULL || !valid_peer(peer)) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    if (unavailable_locked(chord, &peer->id)) { pthread_mutex_unlock(&chord->mutex); errno = EHOSTUNREACH; return -1; }
    if (!node_id_equal(&peer->id, &chord->local.id) && (!chord->has_predecessor || chord_interval_open_open(&chord->predecessor.id, &chord->local.id, &peer->id)))
    {
        chord->predecessor = *peer;
        chord->has_predecessor = 1;
    }
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

int chord_consider_predecessor(Chord *chord, const ChordPeer *candidate)
{
    if (chord == NULL || !valid_peer(candidate)) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    if (unavailable_locked(chord, &candidate->id)) { pthread_mutex_unlock(&chord->mutex); errno = EHOSTUNREACH; return -1; }
    if (!node_id_equal(&candidate->id, &chord->local.id) && (node_id_equal(&chord->successor.id, &chord->local.id) || chord_interval_open_open(&chord->local.id, &chord->successor.id, &candidate->id)))
    {
        chord->successor = *candidate;
        chord->fingers[0] = *candidate;
    }
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

int chord_set_finger(Chord *chord, unsigned index, const ChordPeer *peer)
{
    if (chord == NULL || index >= CHORD_FINGER_COUNT || !valid_peer(peer)) { errno = EINVAL; return -1; }
    if (index == 0U) return chord_set_successor(chord, peer);
    pthread_mutex_lock(&chord->mutex);
    if (unavailable_locked(chord, &peer->id)) { pthread_mutex_unlock(&chord->mutex); errno = EHOSTUNREACH; return -1; }
    chord->fingers[index] = *peer;
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

/* Responde localmente ou indica o salto mais proximo que antecede a chave. */
int chord_route(Chord *chord, const NodeID *key, ChordPeer *next, int *complete)
{
    if (chord == NULL || key == NULL || next == NULL || complete == NULL) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    if (node_id_equal(key, &chord->local.id))
    {
        *next = chord->local;
        *complete = 1;
    }
    else if (node_id_equal(&chord->successor.id, &chord->local.id) || chord_interval_open_closed(&chord->local.id, &chord->successor.id, key))
    {
        *next = chord->successor;
        *complete = 1;
    }
    else
    {
        *next = chord->successor;
        *complete = 0;
        for (unsigned i = CHORD_FINGER_COUNT; i-- > 0U;)
        {
            if (!node_id_equal(&chord->fingers[i].id, &chord->local.id) && chord_interval_open_open(&chord->local.id, key, &chord->fingers[i].id)) { *next = chord->fingers[i]; break; }
        }
    }
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

/* Invalida todas as referências, sem fazer TCP sob o mutex do anel. */
int chord_forget(Chord *chord, const NodeID *id)
{
    if (chord == NULL || id == NULL) { errno = EINVAL; return -1; }
    if (node_id_equal(id, &chord->local.id)) { errno = EPERM; return -1; }
    pthread_mutex_lock(&chord->mutex);
    if (!unavailable_locked(chord, id))
    {
        UnavailableNode *entry = malloc(sizeof(*entry));
        if (entry == NULL) { pthread_mutex_unlock(&chord->mutex); return -1; }
        entry->id = *id; entry->next = chord->unavailable; chord->unavailable = entry;
    }
    if (node_id_equal(&chord->predecessor.id, id)) chord->has_predecessor = 0;
    if (node_id_equal(&chord->successor.id, id)) chord->successor = chord->local;
    for (unsigned i = 0U; i < CHORD_FINGER_COUNT; ++i) if (node_id_equal(&chord->fingers[i].id, id)) chord->fingers[i] = chord->local;
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

/* Somente o integrador de membership chama após evidência direta válida da nova instância. */
int chord_allow(Chord *chord, const NodeID *id)
{
    if (chord == NULL || id == NULL) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    UnavailableNode **slot = &chord->unavailable;
    while (*slot != NULL && !node_id_equal(&(*slot)->id, id)) slot = &(*slot)->next;
    if (*slot != NULL) { UnavailableNode *removed = *slot; *slot = removed->next; free(removed); }
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}

/* Escolhe o menor ID vivo após o local; no wrap-around escolhe o menor absoluto. */
int chord_repair(Chord *chord, const ChordPeer *alive, size_t count)
{
    if (chord == NULL || (count != 0U && alive == NULL)) { errno = EINVAL; return -1; }
    for (size_t i = 0U; i < count; ++i) if (!valid_peer(&alive[i])) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&chord->mutex);
    ChordPeer successor = chord->local;
    int selected = 0;
    for (size_t i = 0U; i < count; ++i)
    {
        if (node_id_equal(&alive[i].id, &chord->local.id)) continue;
        if (unavailable_locked(chord, &alive[i].id)) continue;
        int after = node_id_compare(&alive[i].id, &chord->local.id) > 0;
        int selected_after = node_id_compare(&successor.id, &chord->local.id) > 0;
        if (!selected || (after && !selected_after) || (after == selected_after && node_id_compare(&alive[i].id, &successor.id) < 0)) { successor = alive[i]; selected = 1; }
    }
    chord->successor = successor; chord->fingers[0] = successor;
    pthread_mutex_unlock(&chord->mutex);
    return 0;
}
