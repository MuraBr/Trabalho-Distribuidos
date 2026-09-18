#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

/* Versão aceita, tamanho do identificador de transação e limite de alocação por payload. */
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

/* Códigos de mensagem transmitidos em um byte no header. */
typedef enum m_type
{
    M_JOIN = 0,
    M_ACK = 1,
    M_ERROR = 2,
    M_PING = 3,
    M_PONG = 4,
    M_LEAVE = 5,
} Message_Type;

/* Representação em memória: a serialização define a disposição dos campos na rede. */
typedef struct header
{
    uint8_t protocol_version;
    uint8_t message_type;
    uint8_t source_node[NODE_ID_SIZE]; /* Identidade de quem envia a mensagem. */
    uint8_t destination_node[NODE_ID_SIZE]; /* Destinatário; JOIN inicial pode usar zeros. */
    uint8_t transaction_id[TRANSACTION_ID_SIZE]; /* Relaciona solicitação e resposta. */
    uint64_t timestamp;
    uint32_t payload_size; /* Delimita o corpo no fluxo contínuo de bytes do TCP. */
    uint32_t checksum; /* CRC32 do header com este campo zerado, seguido do corpo. */
} Header;

/* O payload alocado pertence à mensagem e deve ser liberado com message_free. */
typedef struct message
{
    Header header;
    uint8_t *payload;
} Message;

/* Distingue sucesso, erro e fechamento antes do início de uma nova mensagem. */
typedef enum protocol_result
{
    PROTOCOL_ERROR = -1,
    PROTOCOL_OK = 0,
    PROTOCOL_CLOSED = 1,
} Protocol_Result;

/* Zera a mensagem e define a versão; não libera um payload anteriormente alocado. */
int message_init(Message *message);
/* Libera o payload pertencente à mensagem e zera seus campos; aceita ponteiro nulo. */
void message_free(Message *message);

/* Valida e escreve os 98 bytes do header campo a campo, sem transmitir padding da struct. */
int protocol_serialize_header(const Header *header, uint8_t *buffer, size_t buffer_size);
/* Lê os campos dos 98 bytes; a validação semântica do header deve ser feita separadamente. */
int protocol_deserialize_header(Header *header, const uint8_t *buffer, size_t buffer_size);
/* Calcula CRC32 de um buffer; zero também é retorno para ponteiro nulo com tamanho positivo. */
uint32_t protocol_calculate_crc32(const uint8_t *data, size_t size);
/* Exige versão conhecida, tipo permitido e payload de até 4 MiB; não verifica CRC. */
int protocol_validate_header(const Header *header);
/* Valida a mensagem, calcula CRC em uma cópia do header e envia header seguido pelo payload. */
int protocol_send_message(int sock, const Message *message);
/* Lê header e payload completos, valida limites e CRC e entrega o payload alocado ao chamador. */
int protocol_receive_message(int sock, Message *message);

#endif /* PROTOCOL_H */
