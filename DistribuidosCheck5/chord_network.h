#ifndef CHORD_NETWORK_H
#define CHORD_NETWORK_H

#include "chord.h"
#include "protocol.h"

#define CHORD_PEER_WIRE_SIZE JOIN_PAYLOAD_WIRE_SIZE
#define CHORD_ROUTE_WIRE_SIZE (1U + CHORD_PEER_WIRE_SIZE)

int chord_network_handle(Chord *chord, const Message *request, Message_Type *response_type, uint8_t response[CHORD_ROUTE_WIRE_SIZE], uint32_t *response_size);
int chord_network_join(Chord *chord, const char *host, uint16_t port);
int chord_network_find(Chord *chord, const NodeID *key, ChordPeer *output);
int chord_network_maintain(Chord *chord, unsigned finger_index);

#endif
