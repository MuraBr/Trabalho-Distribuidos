#include "chord_network.h"
#include "rpc.h"
#include "network.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* A configuração é imutável após iniciar as threads; o detector valida seus limites. */
static unsigned control_budget(void)
{
    const char *text = getenv("C3_RPC_MS");
    return text == NULL ? 1000U : (unsigned)strtoul(text, NULL, 10);
}

/* Descritores na rede reutilizam o formato validado do JOIN de 64 bytes. */
static int decode_peer(const uint8_t *payload, uint32_t size, ChordPeer *peer)
{
    Node node;
    if (rpc_decode_join_payload(payload, size, &peer->config) < 0 || node_init(&node, &peer->config) < 0) return -1;
    peer->id = node.id;
    return 0;
}

static int encode_peer(const ChordPeer *peer, uint8_t output[CHORD_PEER_WIRE_SIZE])
{
    return rpc_encode_join_payload(&peer->config, output);
}

/* Uma resposta so e aceita se o descritor comprovar o NodeID do emissor. */
static int call_peer(Chord *chord, const ChordPeer *target, Message_Type type, const uint8_t *payload, uint32_t size, Message *response)
{
    ChordPeer local;
    if (chord_local(chord, &local) < 0 || rpc_call_deadline(target->config.ip, target->config.port, &local.id, &target->id, type, payload, size, response, control_budget()) < 0) return -1;
    return 0;
}

/* Resolve cada salto por TCP; um limite finito impede ciclos em anel inconsistente. */
static int find_from(Chord *chord, const ChordPeer *first, const NodeID *key, ChordPeer *output)
{
    ChordPeer current;
    ChordPeer local;
    ChordPeer next;
    NodeID visited[CHORD_FINGER_COUNT + 1U];
    unsigned visited_count = 0U;
    if (chord == NULL || first == NULL || key == NULL || output == NULL || chord_local(chord, &local) < 0) { errno = EINVAL; return -1; }
    current = *first;
    for (unsigned hop = 0U; hop < CHORD_FINGER_COUNT + 1U; ++hop)
    {
        for (unsigned i = 0U; i < visited_count; ++i) if (node_id_equal(&current.id, &visited[i])) { errno = ELOOP; return -1; }
        visited[visited_count++] = current.id;
        int complete;
        if (node_id_equal(&current.id, &local.id))
        {
            if (chord_route(chord, key, &next, &complete) < 0) return -1;
        }
        else
        {
            Message response;
            if (call_peer(chord, &current, M_CHORD_ROUTE, key->bytes, NODE_ID_SIZE, &response) < 0)
            {
                /* Tenta outro salto conhecido, sem declarar FAILED por uma única RPC. */
                ChordPeer alternative; int found = 0;
                for (unsigned i = CHORD_FINGER_COUNT; i-- > 0U;)
                {
                    if (chord_get_finger(chord, i, &alternative) < 0 || node_id_equal(&alternative.id, &local.id)) continue;
                    int seen = 0;
                    for (unsigned j = 0U; j < visited_count; ++j) if (node_id_equal(&alternative.id, &visited[j])) seen = 1;
                    if (!seen) { current = alternative; found = 1; break; }
                }
                if (!found) { errno = EHOSTUNREACH; return -1; }
                continue;
            }
            if (response.header.message_type != M_CHORD_ROUTE || response.header.payload_size != CHORD_ROUTE_WIRE_SIZE || response.payload[0] > 1U || decode_peer(response.payload + 1U, CHORD_PEER_WIRE_SIZE, &next) < 0)
            {
                message_free(&response); errno = EBADMSG; return -1;
            }
            complete = response.payload[0] != 0U;
            message_free(&response);
        }
        if (complete)
        {
            /* O owner também precisa responder: não devolver silenciosamente um nó morto. */
            if (!node_id_equal(&next.id, &local.id))
            {
                Message check;
                if (call_peer(chord, &next, M_CHORD_INFO, NULL, 0U, &check) < 0) { current = next; continue; }
                ChordPeer confirmed;
                int valid = check.header.message_type == M_CHORD_INFO && decode_peer(check.payload, check.header.payload_size, &confirmed) == 0 && node_id_equal(&confirmed.id, &next.id);
                message_free(&check);
                if (!valid) { errno = EBADMSG; return -1; }
            }
            *output = next; return 0;
        }
        if (node_id_equal(&next.id, &current.id)) { errno = ELOOP; return -1; }
        current = next;
    }
    errno = ELOOP;
    return -1;
}

int chord_network_find(Chord *chord, const NodeID *key, ChordPeer *output)
{
    ChordPeer local;
    if (chord_local(chord, &local) < 0) return -1;
    return find_from(chord, &local, key, output);
}

/* O bootstrap informa sua identidade; o lookup da chave local encontra o sucessor. */
int chord_network_join(Chord *chord, const char *host, uint16_t port)
{
    ChordPeer local;
    ChordPeer bootstrap;
    ChordPeer successor;
    Message response;
    if (chord == NULL || host == NULL || chord_local(chord, &local) < 0) { errno = EINVAL; return -1; }
    if (rpc_call_deadline(host, port, &local.id, NULL, M_CHORD_INFO, NULL, 0U, &response, control_budget()) < 0) return -1;
    if (response.header.message_type != M_CHORD_INFO || response.header.payload_size != CHORD_PEER_WIRE_SIZE || decode_peer(response.payload, CHORD_PEER_WIRE_SIZE, &bootstrap) < 0 || memcmp(bootstrap.id.bytes, response.header.source_node, NODE_ID_SIZE) != 0)
    {
        message_free(&response); errno = EBADMSG; return -1;
    }
    message_free(&response);
    if (node_id_equal(&local.id, &bootstrap.id)) { errno = EEXIST; return -1; }
    /* A primeira busca parte do bootstrap; o nó novo ainda não pertence ao anel. */
    if (find_from(chord, &bootstrap, &local.id, &successor) < 0) return -1;
    return chord_set_successor(chord, &successor);
}

/* Stabilize/notify corrigem vizinhos; cada chamada atualiza uma finger entry. */
int chord_network_maintain(Chord *chord, unsigned finger_index)
{
    ChordPeer local;
    ChordPeer successor;
    ChordPeer candidate;
    ChordPeer found;
    NodeID start;
    Message response;
    uint8_t payload[CHORD_PEER_WIRE_SIZE];
    int status = 0;
    if (chord == NULL || finger_index >= CHORD_FINGER_COUNT || chord_local(chord, &local) < 0 || chord_get_successor(chord, &successor) < 0) { errno = EINVAL; return -1; }
    if (!node_id_equal(&successor.id, &local.id))
    {
        if (call_peer(chord, &successor, M_CHORD_PREDECESSOR, NULL, 0U, &response) < 0) return -1;
        if (response.header.message_type != M_CHORD_PREDECESSOR || (response.header.payload_size != 0U && response.header.payload_size != CHORD_PEER_WIRE_SIZE)) { message_free(&response); errno = EBADMSG; return -1; }
        if (response.header.payload_size == CHORD_PEER_WIRE_SIZE)
        {
            if (decode_peer(response.payload, CHORD_PEER_WIRE_SIZE, &candidate) < 0) { message_free(&response); return -1; }
            (void)chord_consider_predecessor(chord, &candidate);
        }
        message_free(&response);
        (void)chord_get_successor(chord, &successor);
        if (encode_peer(&local, payload) < 0 || call_peer(chord, &successor, M_CHORD_NOTIFY, payload, CHORD_PEER_WIRE_SIZE, &response) < 0) return -1;
        status = response.header.message_type == M_ACK && response.header.payload_size == 0U ? 0 : -1;
        message_free(&response);
        if (status < 0) { errno = EBADMSG; return -1; }
    }
    else
    {
        int exists;
        if (chord_get_predecessor(chord, &candidate, &exists) < 0) return -1;
        if (exists) (void)chord_consider_predecessor(chord, &candidate);
    }
    chord_id_add_power(&local.id, finger_index, &start);
    if (chord_network_find(chord, &start, &found) < 0) return -1;
    return chord_set_finger(chord, finger_index, &found);
}

/* Atende somente os tipos privados do overlay; valida tamanhos e identidade do NOTIFY. */
int chord_network_handle(Chord *chord, const Message *request, Message_Type *response_type, uint8_t response[CHORD_ROUTE_WIRE_SIZE], uint32_t *response_size)
{
    ChordPeer local;
    ChordPeer peer;
    NodeID key;
    int complete;
    int exists;
    if (chord == NULL || request == NULL || response_type == NULL || response == NULL || response_size == NULL || chord_local(chord, &local) < 0) { errno = EINVAL; return -1; }
    if (request->header.message_type == M_CHORD_INFO && request->header.payload_size == 0U)
    {
        *response_type = M_CHORD_INFO;
        *response_size = CHORD_PEER_WIRE_SIZE;
        return encode_peer(&local, response);
    }
    if (request->header.message_type == M_CHORD_ROUTE && request->header.payload_size == NODE_ID_SIZE)
    {
        memcpy(key.bytes, request->payload, NODE_ID_SIZE);
        if (chord_route(chord, &key, &peer, &complete) < 0 || encode_peer(&peer, response + 1U) < 0) return -1;
        response[0] = (uint8_t)complete;
        *response_type = M_CHORD_ROUTE;
        *response_size = CHORD_ROUTE_WIRE_SIZE;
        return 0;
    }
    if (request->header.message_type == M_CHORD_PREDECESSOR && request->header.payload_size == 0U)
    {
        if (chord_get_predecessor(chord, &peer, &exists) < 0) return -1;
        *response_type = M_CHORD_PREDECESSOR;
        *response_size = exists ? CHORD_PEER_WIRE_SIZE : 0U;
        return exists ? encode_peer(&peer, response) : 0;
    }
    if (request->header.message_type == M_CHORD_NOTIFY && request->header.payload_size == CHORD_PEER_WIRE_SIZE)
    {
        if (decode_peer(request->payload, CHORD_PEER_WIRE_SIZE, &peer) < 0 || memcmp(peer.id.bytes, request->header.source_node, NODE_ID_SIZE) != 0 || chord_notify(chord, &peer) < 0) { errno = EACCES; return -1; }
        *response_type = M_ACK;
        *response_size = 0U;
        return 0;
    }
    if (request->header.message_type == M_CHORD_FINGER && request->header.payload_size == 1U)
    {
        if (chord_get_finger(chord, request->payload[0], &peer) < 0) return -1;
        *response_type = M_CHORD_FINGER;
        *response_size = CHORD_PEER_WIRE_SIZE;
        return encode_peer(&peer, response);
    }
    errno = EINVAL;
    return -1;
}
