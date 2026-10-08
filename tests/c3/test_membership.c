#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "superpeer.h"
#include "gossip.h"
#include "chord.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Tempos injetados: os limites são verificados sem aguardar segundos reais. */
int main(void)
{
    SuperPeerConfig config; SuperPeer *table; Node local, peer;
    uint8_t uuid[16] = {1U};
    assert(superpeer_config_init_with_uuid(&config, "127.0.0.1", 55001U, uuid) == 0);
    assert(superpeer_create(&config, &table) == 0);
    assert(superpeer_get_node(table, &local) == 0);
    uuid[0] = 2U;
    NodeConfig remote;
    assert(node_config_init_with_uuid(&remote, "127.0.0.1", 55002U, uuid) == 0);
    assert(node_init(&peer, &remote) == 0);
    assert(membership_register(table, &peer, &local.id, 1U, 0) == SUPERPEER_MEMBER_ADDED);
    SuperPeerMember m;
    assert(membership_tick(table, 14999, 15000U, 20000U, 30000U) == 0);
    assert(superpeer_find_member(table, &peer.id, &m) == 0 && m.state == SUPERPEER_MEMBER_ALIVE);
    assert(membership_tick(table, 15000, 15000U, 20000U, 30000U) == 0);
    assert(superpeer_find_member(table, &peer.id, &m) == 0 && m.state == SUPERPEER_MEMBER_SUSPECT);
    SuperPeerMember heartbeat = m; heartbeat.state = SUPERPEER_MEMBER_ALIVE; heartbeat.sequence = 1U;
    assert(membership_heartbeat(table, &heartbeat, 15001) == 0);
    assert(membership_heartbeat(table, &heartbeat, 29000) == -1 && errno == ESTALE);
    assert(membership_tick(table, 30001, 15000U, 20000U, 30000U) == 0);
    assert(membership_tick(table, 35001, 15000U, 20000U, 30000U) == 0);
    assert(superpeer_find_member(table, &peer.id, &m) == 0 && m.state == SUPERPEER_MEMBER_SUSPECT);
    assert(membership_probe(table, &peer.id, 1U, 1U, 31000, 0) == 0);
    assert(membership_probe(table, &peer.id, 1U, 1U, 31500, 0) == 0);
    assert(membership_tick(table, 35001, 15000U, 20000U, 30000U) == 0);
    assert(superpeer_find_member(table, &peer.id, &m) == 0 && m.state == SUPERPEER_MEMBER_SUSPECT);
    assert(membership_probe(table, &peer.id, 1U, 1U, 32000, 0) == 0);
    assert(membership_tick(table, 35001, 15000U, 20000U, 30000U) == 0);
    assert(superpeer_find_member(table, &peer.id, &m) == 0 && m.state == SUPERPEER_MEMBER_FAILED);
    assert(!superpeer_is_registered(table, &peer.id));
    assert(membership_tick(table, 45001, 15000U, 20000U, 30000U) == 0);
    assert(superpeer_find_member(table, &peer.id, &m) < 0);
    assert(membership_register(table, &peer, &local.id, 1U, 46000) == SUPERPEER_REGISTER_ERROR);
    /* Um ALIVE atrasado, mesmo com sequência elevada, não remove o tombstone. */
    heartbeat.sequence = 999U; heartbeat.reporter = local.id;
    assert(membership_merge(table, &heartbeat) == 0);
    assert(superpeer_find_member(table, &peer.id, &m) < 0);
    char directory[] = "/tmp/pd-membership-unit.XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char path[200]; snprintf(path, sizeof(path), "%s/membership.bin", directory);
    assert(membership_save(table, path) == 0);
    SuperPeer *restored; assert(superpeer_create(&config, &restored) == 0);
    assert(membership_load(restored, path) == 0);
    assert(membership_register(restored, &peer, &local.id, 1U, 0) == SUPERPEER_REGISTER_ERROR);
    assert(membership_register(restored, &peer, &local.id, 2U, 0) == SUPERPEER_MEMBER_UPDATED);
    assert(superpeer_find_member(restored, &peer.id, &m) == 0 && m.incarnation == 2U);
    uint8_t wire[MEMBERSHIP_WIRE_SIZE]; SuperPeerMember decoded;
    assert(gossip_encode_record(&m, wire) == 0 && gossip_decode_record(wire, &decoded) == 0);
    assert(node_id_equal(&decoded.node.id, &m.node.id));
    wire[0] ^= 1U; assert(gossip_decode_record(wire, &decoded) < 0);
    assert(superpeer_unregister_node(restored, &local.id) < 0 && errno == EPERM);
    /* Todas as fingers são invalidadas e a escolha do sucessor usa ordem circular. */
    Chord *chord; assert(chord_create(&local, &chord) == 0);
    ChordPeer candidate = {peer.id, peer.config}, got;
    assert(chord_set_successor(chord, &candidate) == 0);
    assert(chord_set_finger(chord, 255U, &candidate) == 0);
    assert(chord_forget(chord, &peer.id) == 0);
    assert(chord_get_finger(chord, 255U, &got) == 0 && node_id_equal(&got.id, &local.id));
    /* Respostas da instância morta não podem repovoar vizinhos nem fingers. */
    assert(chord_set_finger(chord, 255U, &candidate) < 0);
    assert(chord_set_successor(chord, &candidate) < 0);
    assert(chord_notify(chord, &candidate) < 0);
    assert(chord_consider_predecessor(chord, &candidate) < 0);
    assert(chord_forget(chord, &local.id) < 0 && errno == EPERM);
    assert(chord_repair(chord, &candidate, 1U) == 0);
    assert(chord_get_successor(chord, &got) == 0 && node_id_equal(&got.id, &local.id));
    assert(chord_allow(chord, &peer.id) == 0);
    assert(chord_repair(chord, &candidate, 1U) == 0);
    assert(chord_get_successor(chord, &got) == 0 && node_id_equal(&got.id, &peer.id));
    chord_destroy(chord); superpeer_destroy(restored); superpeer_destroy(table);
    unlink(path); rmdir(directory);
    puts("Membership C3: limites, confirmacao, recuperacao, tombstones, wire e reparo OK");
    return 0;
}
