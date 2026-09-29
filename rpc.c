#include "rpc.h"

#include "network.h"
#include "transfer_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int rpc_encode_join_payload(const NodeConfig *config, uint8_t output[JOIN_PAYLOAD_WIRE_SIZE])
{
    uint16_t port;

    if (config == NULL || output == NULL || node_config_validate(config) < 0)
    {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, JOIN_PAYLOAD_WIRE_SIZE);
    memcpy(output, config->ip, NODE_ADDRESS_SIZE);
    port = htons(config->port);
    memcpy(output + NODE_ADDRESS_SIZE, &port, sizeof(port));
    memcpy(output + NODE_ADDRESS_SIZE + sizeof(port), config->uuid, NODE_UUID_SIZE);
    return 0;
}

int rpc_call(const char *host, uint16_t port, const NodeID *source, const NodeID *destination, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message *response)
{
    Message request;
    int socket_fd;
    int status = -1;

    if (host == NULL || response == NULL || payload_size > MAX_PAYLOAD_SIZE || (payload_size != 0U && payload == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    if (message_init(&request) < 0 || message_init(response) < 0)
    {
        return -1;
    }
    socket_fd = network_connect(host, port);
    if (socket_fd < 0)
    {
        return -1;
    }
    request.header.message_type = (uint8_t)type;
    if (source != NULL)
    {
        memcpy(request.header.source_node, source->bytes, NODE_ID_SIZE);
    }
    if (destination != NULL)
    {
        memcpy(request.header.destination_node, destination->bytes, NODE_ID_SIZE);
    }
    transfer_fill_transaction_id(request.header.transaction_id, request.header.source_node);
    request.header.timestamp = (uint64_t)time(NULL);
    request.header.payload_size = payload_size;
    request.payload = (uint8_t *)payload;
    errno = 0;
    if (protocol_send_message(socket_fd, &request) != PROTOCOL_OK || protocol_receive_message(socket_fd, response) != PROTOCOL_OK)
    {
        if (errno == 0) errno = EBADMSG;
        goto cleanup;
    }
    if (memcmp(request.header.transaction_id, response->header.transaction_id, TRANSACTION_ID_SIZE) != 0)
    {
        errno = EBADMSG;
        goto cleanup;
    }
    status = 0;
    if ((source != NULL && memcmp(response->header.destination_node, source->bytes, NODE_ID_SIZE) != 0) || (destination != NULL && memcmp(response->header.source_node, destination->bytes, NODE_ID_SIZE) != 0))
    {
        errno = EBADMSG;
        status = -1;
    }

cleanup:
    request.payload = NULL;
    { int error = errno; (void)network_shutdown(socket_fd); errno = error; }
    if (status < 0)
    {
        message_free(response);
    }
    return status;
}

int rpc_decode_join_payload(const uint8_t *payload, size_t payload_size, NodeConfig *config)
{
    char ip[NODE_ADDRESS_SIZE];
    const uint8_t *terminator;
    uint16_t network_port;
    uint16_t port;
    size_t index;

    if (payload == NULL || config == NULL || payload_size != JOIN_PAYLOAD_WIRE_SIZE)
    {
        errno = EINVAL;
        return -1;
    }

    terminator = memchr(payload, '\0', NODE_ADDRESS_SIZE);
    if (terminator == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    /* O preenchimento após o terminador deve conter apenas zeros. */
    index = (size_t)(terminator - payload) + 1U;
    while (index < NODE_ADDRESS_SIZE)
    {
        if (payload[index] != 0U)
        {
            errno = EINVAL;
            return -1;
        }
        ++index;
    }

    memcpy(ip, payload, sizeof(ip));
    memcpy(&network_port, payload + NODE_ADDRESS_SIZE, sizeof(network_port));
    port = ntohs(network_port);

    if (node_config_init_with_uuid( config, ip, port, payload + NODE_ADDRESS_SIZE + sizeof(network_port)) < 0)
    {
        return -1;
    }
    return 0;
}
