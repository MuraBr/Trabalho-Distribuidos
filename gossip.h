#ifndef GOSSIP_H
#define GOSSIP_H
#include "superpeer.h"
#define MEMBERSHIP_WIRE_SIZE 186U
#define GOSSIP_BATCH_MAX 64U
int gossip_encode_record(const SuperPeerMember *member, uint8_t output[MEMBERSHIP_WIRE_SIZE]);
int gossip_decode_record(const uint8_t input[MEMBERSHIP_WIRE_SIZE], SuperPeerMember *member);
#endif
