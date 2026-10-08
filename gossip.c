#include "gossip.h"
#include "wire.h"
#include <errno.h>
#include <string.h>

/* Formato fixo: ID, descritor JOIN, papel, domínio, época, sequência, versão, estado, observador. */
int gossip_encode_record(const SuperPeerMember *member, uint8_t output[MEMBERSHIP_WIRE_SIZE])
{
    if (member == NULL || output == NULL || node_validate(&member->node) < 0 || member->state > SUPERPEER_MEMBER_REMOVED) { errno = EINVAL; return -1; }
    memset(output, 0, MEMBERSHIP_WIRE_SIZE);
    memcpy(output, member->node.id.bytes, 32U);
    memcpy(output + 32U, member->node.config.ip, NODE_ADDRESS_SIZE);
    wire_put_u16(output + 78U, member->node.config.port);
    memcpy(output + 80U, member->node.config.uuid, 16U);
    output[96] = (uint8_t)member->node.role;
    memcpy(output + 97U, member->home.bytes, 32U);
    wire_put_u64(output + 129U, member->incarnation);
    wire_put_u64(output + 137U, member->sequence);
    wire_put_u64(output + 145U, member->version);
    output[153] = (uint8_t)member->state;
    memcpy(output + 154U, member->reporter.bytes, 32U);
    return 0;
}

int gossip_decode_record(const uint8_t input[MEMBERSHIP_WIRE_SIZE], SuperPeerMember *member)
{
    if (input == NULL || member == NULL) { errno = EINVAL; return -1; }
    memset(member, 0, sizeof(*member));
    const uint8_t *end = memchr(input + 32U, 0, NODE_ADDRESS_SIZE);
    if (end == NULL || input[96] > NODE_ROLE_SUPERPEER || input[153] > SUPERPEER_MEMBER_REMOVED) { errno = EBADMSG; return -1; }
    for (const uint8_t *p = end; p < input + 78U; ++p) if (*p != 0U) { errno = EBADMSG; return -1; }
    NodeConfig config;
    char ip[NODE_ADDRESS_SIZE];
    memcpy(ip, input + 32U, sizeof(ip));
    if (node_config_init_with_uuid(&config, ip, wire_get_u16(input + 78U), input + 80U) < 0 || node_init(&member->node, &config) < 0 || memcmp(member->node.id.bytes, input, 32U) != 0) { errno = EBADMSG; return -1; }
    member->node.role = (NodeRole)input[96];
    memcpy(member->home.bytes, input + 97U, 32U);
    member->incarnation = wire_get_u64(input + 129U);
    member->sequence = wire_get_u64(input + 137U);
    member->version = wire_get_u64(input + 145U);
    member->state = (SuperPeerMemberState)input[153];
    memcpy(member->reporter.bytes, input + 154U, 32U);
    return 0;
}
