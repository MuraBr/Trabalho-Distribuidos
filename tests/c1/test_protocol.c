#include "network.h"
#include "protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Preenche um TransactionID fixo para tornar a comparação do teste reproduzível. */
static void fill_transaction_id(uint8_t transaction_id[TRANSACTION_ID_SIZE])
{
    size_t index;

    for (index = 0U; index < TRANSACTION_ID_SIZE; ++index)
    {
        transaction_id[index] = (uint8_t)(index + 1U);
    }
}

/* Usa socketpair local para conferir envio e recepção com payload; não testa conexão TCP real. */
static void test_message_round_trip(void)
{
    static const uint8_t payload[] = {'c', '1', '-', 'o', 'k'};
    Message sent;
    Message received;
    int sockets[2];
    uint8_t *sent_payload;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(message_init(&sent) == PROTOCOL_OK);
    assert(message_init(&received) == PROTOCOL_OK);

    sent_payload = malloc(sizeof(payload));
    assert(sent_payload != NULL);
    memcpy(sent_payload, payload, sizeof(payload));

    sent.header.message_type = (uint8_t)M_PING;
    memcpy(sent.header.source_node, "source", 6U);
    memcpy(sent.header.destination_node, "dest", 4U);
    fill_transaction_id(sent.header.transaction_id);
    sent.header.timestamp = 123456U;
    sent.header.payload_size = sizeof(payload);
    sent.payload = sent_payload;

    assert(protocol_send_message(sockets[0], &sent) == PROTOCOL_OK);
    assert(protocol_receive_message(sockets[1], &received) == PROTOCOL_OK);
    assert(received.header.message_type == (uint8_t)M_PING);
    assert(received.header.timestamp == sent.header.timestamp);
    assert(received.header.payload_size == sizeof(payload));
    assert(memcmp(received.header.transaction_id, sent.header.transaction_id, TRANSACTION_ID_SIZE) == 0);
    assert(memcmp(received.payload, payload, sizeof(payload)) == 0);

    message_free(&sent);
    message_free(&received);
    assert(close(sockets[0]) == 0);
    assert(close(sockets[1]) == 0);
}

/* Executa o teste de ida e volta; assert encerra o processo se uma condição falhar. */
int main(void)
{
    test_message_round_trip();
    puts("protocol tests: ok");
    return 0;
}
