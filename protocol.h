#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_VERSION 1u
#define TRANSACTION_ID_SIZE 16u
#define MAX_PAYLOAD_SIZE (4u * 1024u * 1024u)

/*
 * Layout do payload de JOIN e ACK usado na integracao:
 * [46 bytes de IP textual, incluindo terminador NUL e padding]
 * [2 bytes de porta em ordem de rede]
 * [16 bytes de UUID]
 */
#define JOIN_PAYLOAD_WIRE_SIZE (NODE_ADDRESS_SIZE + 2u + NODE_UUID_SIZE)

/*
 * O header e serializado manualmente. Nao use sizeof(Header) para
 * determinar o tamanho enviado pela rede, pois a struct pode ter padding.
 */
#define HEADER_WIRE_SIZE 98u

typedef enum m_type
{
    M_JOIN = 0,
    M_ACK = 1,
    M_ERROR = 2,
    M_PING = 3,
    M_PONG = 4,
    M_LEAVE = 5,
} Message_Type;

typedef struct header
{
    uint8_t protocol_version;
    uint8_t message_type;
    uint8_t source_node[NODE_ID_SIZE];
    uint8_t destination_node[NODE_ID_SIZE];
    uint8_t transaction_id[TRANSACTION_ID_SIZE];
    uint64_t timestamp;
    uint32_t payload_size;
    uint32_t checksum;
} Header;

typedef struct message
{
    Header header;
    uint8_t *payload;
} Message;

typedef enum protocol_result
{
    PROTOCOL_ERROR = -1,
    PROTOCOL_OK = 0,
    PROTOCOL_CLOSED = 1,
} Protocol_Result;

int message_init(Message *message);
void message_free(Message *message);

int protocol_serialize_header(const Header *header, uint8_t *buffer, size_t buffer_size);
int protocol_deserialize_header(Header *header, const uint8_t *buffer, size_t buffer_size);
uint32_t protocol_calculate_crc32(const uint8_t *data, size_t size);
int protocol_validate_header(const Header *header);
int protocol_send_message(int sock, const Message *message);
int protocol_receive_message(int sock, Message *message);

#endif /* PROTOCOL_H */
