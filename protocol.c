#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum m_type
{
    JOIN,
    ACK,
    ERROR,
} Message_Type;

typedef struct header
{
    uint8_t protocol_version;
    uint8_t message_type;
    void *source_node;
    void *destination_node;
    void *transaction_id;
    void *timestamp;
    void *payload_size;
    void *checksum;

} Header;


void message_init()
{
}

void message_free()
{
}

void protocol_serialize_header()
{
}

void protocol_deserialize_header()
{
}

void protocol_calculate_crc32()
{
}

void protocol_validate_header()
{
}

void protocol_send_message()
{
}

void protocol_receive_message()
{
}
