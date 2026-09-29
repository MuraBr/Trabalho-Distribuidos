#ifndef RPC_H
#define RPC_H

#include "node.h"
#include "protocol.h"

int rpc_call(const char *host, uint16_t port, const NodeID *source, const NodeID *destination, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message *response);
int rpc_encode_join_payload(const NodeConfig *config, uint8_t output[JOIN_PAYLOAD_WIRE_SIZE]);

#endif
