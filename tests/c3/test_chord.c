#include "chord.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* Confere aritmetica de 256 bits, inclusive retorno ao inicio do anel. */
static void test_powers_and_intervals(void)
{
    NodeID base = {{0}};
    NodeID result;
    NodeID end = {{0}};
    NodeID key = {{0}};
    chord_id_add_power(&base, 0U, &result);
    assert(result.bytes[31] == 1U);
    chord_id_add_power(&base, 255U, &result);
    assert(result.bytes[0] == 128U);
    memset(base.bytes, 255, NODE_ID_SIZE);
    chord_id_add_power(&base, 0U, &result);
    assert(result.bytes[0] == 0U && result.bytes[31] == 0U);
    base.bytes[31] = 250U;
    end.bytes[31] = 5U;
    key.bytes[31] = 2U;
    assert(chord_interval_open_closed(&base, &end, &key));
    key.bytes[31] = 200U;
    assert(!chord_interval_open_closed(&base, &end, &key));
}

/* Um anel isolado resolve qualquer chave para si mesmo. */
static void test_single_node(void)
{
    Node local = {0};
    NodeID key = {{0}};
    Chord *chord;
    ChordPeer next;
    int complete;
    local.id.bytes[31] = 42U;
    strcpy(local.config.ip, "127.0.0.1");
    local.config.port = 56001U;
    assert(chord_create(&local, &chord) == 0);
    key.bytes[31] = 99U;
    assert(chord_route(chord, &key, &next, &complete) == 0);
    assert(complete == 1 && node_id_equal(&next.id, &local.id));
    chord_destroy(chord);
}

int main(void)
{
    test_powers_and_intervals();
    test_single_node();
    puts("Chord local: OK");
    return 0;
}
