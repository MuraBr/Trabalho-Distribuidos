#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "heartbeat.h"
#include "gossip.h"
#include "rpc.h"
#include "network.h"
#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Endpoint que aceita TCP mas não responde; a RPC deve respeitar o orçamento total. */
static void *silent_server(void *context)
{
    int fd = *(int *)context;
    int client = accept(fd, NULL, NULL);
    assert(client >= 0);
    uint8_t data[512];
    while (recv(client, data, sizeof(data), 0) > 0) {}
    close(client);
    return NULL;
}

int main(void)
{
    SuperPeerConfig config; SuperPeer *table; Node local, peer;
    uint8_t uuid[16] = {9U};
    assert(superpeer_config_init_with_uuid(&config, "127.0.0.1", 54901U, uuid) == 0);
    assert(superpeer_create(&config, &table) == 0 && superpeer_get_node(table, &local) == 0);
    char directory[] = "/tmp/pd-control-unit.XXXXXX"; assert(mkdtemp(directory));
    Heartbeat *control;
    assert(heartbeat_create(&local, table, directory, NULL, NULL, &control) == 0);
    uuid[0] = 10U;
    NodeConfig peer_config;
    assert(node_config_init_with_uuid(&peer_config, "127.0.0.1", 54902U, uuid) == 0 && node_init(&peer, &peer_config) == 0);
    assert(heartbeat_register(control, &peer, 1U) == 0);
    SuperPeerMember sender = {.node = peer, .home = local.id, .reporter = local.id, .incarnation = 1U, .sequence = 1U};
    uint8_t payload[HEARTBEAT_WIRE_SIZE] = {1U};
    assert(gossip_encode_record(&sender, payload + 1U) == 0);
    Message request;
    assert(message_init(&request) == 0);
    request.header.message_type = M_HEARTBEAT; request.header.payload_size = sizeof(payload); request.payload = payload;
    memcpy(request.header.source_node, peer.id.bytes, 32U); memcpy(request.header.destination_node, local.id.bytes, 32U);
    uint8_t *response = NULL; uint32_t size; Message_Type type;
    assert(heartbeat_handle(control, &request, &type, &response, &size) == 0 && type == M_HEARTBEAT && size == HEARTBEAT_WIRE_SIZE);
    free(response);
    assert(heartbeat_handle(control, &request, &type, &response, &size) < 0);
    request.header.source_node[0] ^= 1U;
    assert(heartbeat_handle(control, &request, &type, &response, &size) < 0);
    request.header.source_node[0] ^= 1U;
    payload[0] = 99U;
    assert(heartbeat_handle(control, &request, &type, &response, &size) < 0);
    /* Gossip duplicado/antigo é idempotente e não renova relógio de Peer remoto. */
    Node reporter, foreign;
    uuid[0] = 11U;
    assert(node_config_init_with_uuid(&peer_config, "127.0.0.1", 54903U, uuid) == 0 && node_init(&reporter, &peer_config) == 0);
    reporter.role = NODE_ROLE_SUPERPEER;
    uuid[0] = 12U;
    assert(node_config_init_with_uuid(&peer_config, "127.0.0.1", 54904U, uuid) == 0 && node_init(&foreign, &peer_config) == 0);
    SuperPeerMember sp = {.node = reporter, .home = reporter.id, .reporter = reporter.id, .incarnation = 1U, .sequence = 1U};
    SuperPeerMember event = {.node = foreign, .home = reporter.id, .reporter = reporter.id, .incarnation = 1U, .state = SUPERPEER_MEMBER_SUSPECT, .version = 2U};
    uint8_t gossip[HEARTBEAT_WIRE_SIZE + 2U + MEMBERSHIP_WIRE_SIZE] = {1U};
    gossip[HEARTBEAT_WIRE_SIZE + 1U] = 1U;
    assert(gossip_encode_record(&sp, gossip + 1U) == 0 && gossip_encode_record(&event, gossip + HEARTBEAT_WIRE_SIZE + 2U) == 0);
    request.header.message_type = M_GOSSIP; request.header.payload_size = sizeof(gossip); request.payload = gossip;
    memcpy(request.header.source_node, reporter.id.bytes, 32U);
    assert(heartbeat_handle(control, &request, &type, &response, &size) == 0); free(response);
    SuperPeerMember initial, updated;
    assert(superpeer_find_member(table, &foreign.id, &initial) == 0 && initial.state == SUPERPEER_MEMBER_SUSPECT && !initial.local_registration);
    sp.sequence = 2U; assert(gossip_encode_record(&sp, gossip + 1U) == 0);
    assert(heartbeat_handle(control, &request, &type, &response, &size) == 0); free(response);
    event.state = SUPERPEER_MEMBER_ALIVE; event.version = 1U;
    sp.sequence = 3U;
    assert(gossip_encode_record(&sp, gossip + 1U) == 0 && gossip_encode_record(&event, gossip + HEARTBEAT_WIRE_SIZE + 2U) == 0);
    assert(heartbeat_handle(control, &request, &type, &response, &size) == 0); free(response);
    assert(superpeer_find_member(table, &foreign.id, &updated) == 0 && updated.state == initial.state && updated.observed_ms == initial.observed_ms);
    /* Um registro corrompido impede até a aplicação do sender do lote. */
    sp.sequence = 4U; assert(gossip_encode_record(&sp, gossip + 1U) == 0);
    gossip[HEARTBEAT_WIRE_SIZE + 2U] ^= 1U;
    assert(heartbeat_handle(control, &request, &type, &response, &size) < 0);
    assert(superpeer_find_member(table, &reporter.id, &updated) == 0 && updated.sequence == 3U);
    /* Rumor de recuperação não herda a confirmação TCP da instância/estado anterior. */
    SuperPeerMember rumor = updated;
    rumor.state = SUPERPEER_MEMBER_FAILED; ++rumor.version;
    assert(membership_merge(table, &rumor) == 0);
    rumor.state = SUPERPEER_MEMBER_ALIVE; ++rumor.sequence;
    assert(membership_merge(table, &rumor) == 0);
    assert(superpeer_find_member(table, &reporter.id, &updated) == 0 && updated.last_seen == 0);
    request.payload = NULL;
    uint64_t first = heartbeat_incarnation(control);
    heartbeat_destroy(control); superpeer_destroy(table);
    assert(superpeer_create(&config, &table) == 0);
    assert(heartbeat_create(&local, table, directory, NULL, NULL, &control) == 0 && heartbeat_incarnation(control) > first);
    heartbeat_destroy(control); superpeer_destroy(table);
    int server = network_create_server(0U, 4); assert(server >= 0);
    struct sockaddr_in address; socklen_t length = sizeof(address);
    assert(getsockname(server, (struct sockaddr *)&address, &length) == 0);
    pthread_t thread; assert(pthread_create(&thread, NULL, silent_server, &server) == 0);
    Message reply;
    int64_t started = network_monotonic_ms();
    assert(rpc_call_deadline("127.0.0.1", ntohs(address.sin_port), &local.id, NULL, M_PING, (const uint8_t *)"PING", 4U, &reply, 200U) < 0);
    int64_t elapsed = network_monotonic_ms() - started;
    assert(elapsed >= 150 && elapsed < 1500);
    pthread_join(thread, NULL); close(server);
    const char *files[] = {"membership.bin", "node.incarnation", "node.incarnation.lock"};
    for (size_t i = 0U; i < sizeof(files) / sizeof(files[0]); ++i) { char path[256]; snprintf(path, sizeof(path), "%s/%s", directory, files[i]); unlink(path); }
    rmdir(directory);
    puts("Controle C3: identidade, replay, versao, epoch persistente e deadline TCP OK");
    return 0;
}
