#include "protocol.h"

#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <zlib.h>

static void write_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) |
	((uint32_t)source[1] << 16) |
	((uint32_t)source[2] << 8) |
	(uint32_t)source[3];
}

static void write_u64_be(uint8_t *destination, uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        *destination++ = (uint8_t)(value >> shift);
    }
}

static uint64_t read_u64_be(const uint8_t *source)
{
    uint64_t value = 0;

    for (size_t index = 0; index < sizeof(value); index++)
    {
        value = (value << 8) | source[index];
    }

    return value;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    uLong result = (uLong)crc;

    while (size > 0U)
    {
        uInt chunk = size > (size_t)UINT_MAX ? UINT_MAX : (uInt)size;

        result = crc32(result, data, chunk);
        data += chunk;
        size -= chunk;
    }

    return (uint32_t)result;
}

static uint32_t protocol_message_crc32(const Header *header, const uint8_t *payload)
{
    Header canonical_header = *header;
    uint8_t serialized_header[HEADER_WIRE_SIZE];

    canonical_header.checksum = 0;

    if (protocol_serialize_header(&canonical_header, serialized_header, sizeof(serialized_header)) < 0)
    {
        return 0;
    }

    uint32_t crc = crc32_update(0U, serialized_header, sizeof(serialized_header));

    if (canonical_header.payload_size > 0)
    {
        crc = crc32_update(crc, payload, canonical_header.payload_size);
    }

    return crc;
}

int message_init(Message *message)
{
    if (message == NULL)
    {
        return PROTOCOL_ERROR;
    }

    memset(message, 0, sizeof(*message));
    message->header.protocol_version = PROTOCOL_VERSION;
    return PROTOCOL_OK;
}

void message_free(Message *message)
{
    if (message == NULL)
    {
        return;
    }

    free(message->payload);
    message->payload = NULL;
    memset(&message->header, 0, sizeof(message->header));
}

int protocol_serialize_header(const Header *header,
                              uint8_t *buffer,
                              size_t buffer_size)
{
    if (header == NULL || buffer == NULL || buffer_size < HEADER_WIRE_SIZE)
    {
        return PROTOCOL_ERROR;
    }

    if (protocol_validate_header(header) < 0)
    {
        return PROTOCOL_ERROR;
    }

    size_t offset = 0;

    buffer[offset++] = header->protocol_version;
    buffer[offset++] = header->message_type;

    memcpy(buffer + offset, header->source_node, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(buffer + offset, header->destination_node, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(buffer + offset, header->transaction_id, TRANSACTION_ID_SIZE);
    offset += TRANSACTION_ID_SIZE;

    write_u64_be(buffer + offset, header->timestamp);
    offset += sizeof(uint64_t);

    write_u32_be(buffer + offset, header->payload_size);
    offset += sizeof(uint32_t);

    write_u32_be(buffer + offset, header->checksum);
    offset += sizeof(uint32_t);

    return (int)offset;
}

int protocol_deserialize_header(Header *header,
                                const uint8_t *buffer,
                                size_t buffer_size)
{
    if (header == NULL || buffer == NULL || buffer_size < HEADER_WIRE_SIZE)
    {
        return PROTOCOL_ERROR;
    }

    size_t offset = 0;

    memset(header, 0, sizeof(*header));

    header->protocol_version = buffer[offset++];
    header->message_type = buffer[offset++];

    memcpy(header->source_node, buffer + offset, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(header->destination_node, buffer + offset, NODE_ID_SIZE);
    offset += NODE_ID_SIZE;

    memcpy(header->transaction_id, buffer + offset, TRANSACTION_ID_SIZE);
    offset += TRANSACTION_ID_SIZE;

    header->timestamp = read_u64_be(buffer + offset);
    offset += sizeof(uint64_t);

    header->payload_size = read_u32_be(buffer + offset);
    offset += sizeof(uint32_t);

    header->checksum = read_u32_be(buffer + offset);
    offset += sizeof(uint32_t);

    return (int)offset;
}

uint32_t protocol_calculate_crc32(const uint8_t *data, size_t size)
{
    if (data == NULL && size > 0)
    {
        return 0;
    }

    return crc32_update(0U, data, size);
}

int protocol_validate_header(const Header *header)
{
    if (header == NULL)
    {
        return PROTOCOL_ERROR;
    }

    if (header->protocol_version != PROTOCOL_VERSION)
    {
        return PROTOCOL_ERROR;
    }

    switch (header->message_type)
    {
    case M_JOIN:
    case M_ACK:
    case M_ERROR:
    case M_PING:
    case M_PONG:
    case M_LEAVE:
        break;
    default:
        return PROTOCOL_ERROR;
    }

    if (header->payload_size > MAX_PAYLOAD_SIZE)
    {
        return PROTOCOL_ERROR;
    }

    return PROTOCOL_OK;
}

int protocol_send_message(int sock, const Message *message)
{
    if (message == NULL ||
        protocol_validate_header(&message->header) < 0 ||
        (message->header.payload_size > 0 && message->payload == NULL))
    {
        return PROTOCOL_ERROR;
    }

    Header wire_header = message->header;
    uint8_t serialized_header[HEADER_WIRE_SIZE];

    wire_header.checksum = 0;
    wire_header.checksum = protocol_message_crc32(&wire_header,
                                                  message->payload);

    if (protocol_serialize_header(&wire_header,
                                  serialized_header,
                                  sizeof(serialized_header)) < 0)
    {
        return PROTOCOL_ERROR;
    }

    if (network_send_all(sock,
                         serialized_header,
                         sizeof(serialized_header)) !=
        (ssize_t)sizeof(serialized_header))
    {
        return PROTOCOL_ERROR;
    }

    if (wire_header.payload_size > 0 &&
        network_send_all(sock,
                         message->payload,
                         wire_header.payload_size) !=
	(ssize_t)wire_header.payload_size)
    {
        return PROTOCOL_ERROR;
    }

    return PROTOCOL_OK;
}

int protocol_receive_message(int sock, Message *message)
{
    if (message == NULL)
    {
        return PROTOCOL_ERROR;
    }

    uint8_t serialized_header[HEADER_WIRE_SIZE];
    Header received_header;
    ssize_t received = network_recv_exact(sock,
                                          serialized_header,
                                          sizeof(serialized_header));

    if (received == 0)
    {
        return PROTOCOL_CLOSED;
    }

    if (received < 0 || received != (ssize_t)sizeof(serialized_header))
    {
        return PROTOCOL_ERROR;
    }

    if (protocol_deserialize_header(&received_header,
                                    serialized_header,
                                    sizeof(serialized_header)) < 0 ||
        protocol_validate_header(&received_header) < 0)
    {
        return PROTOCOL_ERROR;
    }

    uint8_t *payload = NULL;

    if (received_header.payload_size > 0)
    {
        payload = malloc(received_header.payload_size);
        if (payload == NULL)
        {
            return PROTOCOL_ERROR;
        }

        received = network_recv_exact(sock,
                                      payload,
                                      received_header.payload_size);

        if (received < 0 ||
            received != (ssize_t)received_header.payload_size)
        {
            free(payload);
            return PROTOCOL_ERROR;
        }
    }

    uint32_t expected_checksum = received_header.checksum;
    uint32_t actual_checksum = protocol_message_crc32(&received_header,
                                                      payload);

    if (expected_checksum != actual_checksum)
    {
        free(payload);
        return PROTOCOL_ERROR;
    }

    /* O chamador deve liberar uma mensagem anterior antes de reutilizar o objeto. */
    message->header = received_header;
    message->payload = payload;

    return PROTOCOL_OK;
}
