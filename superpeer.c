#define _POSIX_C_SOURCE 200809L
#include "superpeer.h"
#include "heartbeat.h"
#include "wire.h"

/* Aplicação Super Peer; a API local e seus testes estão separados em membership.c. */

#include "concurrent_server.h"
#include "chord_network.h"
#include "app_config.h"
#include "directory.h"
#include "metadata.h"
#include "network.h"
#include "node.h"
#include "protocol.h"
#include "rpc.h"
#include "remote_error.h"
#include "superpeer.h"
#include "transfer_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define PEER_BACKLOG 10
#define DEFAULT_LOCAL_IP "127.0.0.1"
#define PING_PAYLOAD "PING"
#define PONG_PAYLOAD "PONG"

/* Estado compartilhado: identidade, cadastro de membros e sincronização das conexões. */
typedef struct
{
    int server_fd;
    ConcurrentServer *runtime;
    Node local_node;
    SuperPeer *superpeer;
    MetadataStore *metadata;
    Directory *directory;
    Chord *chord;
    Heartbeat *heartbeat;
} PeerContext;

typedef struct
{
    uint16_t local_port;
    const char *remote_ip;
    uint16_t remote_port;
    const char *config_path;
    const char *node_name;
    const char *chord_host;
    uint16_t chord_port;
    Message_Type command_type;
    int command_mode;
} NodeArguments; /* Opções de inicialização; config_path ainda não tem conteúdo interpretado. */

static volatile sig_atomic_t g_running = 1;

/* Compõe 16 bytes com horário, PID e sequência local para correlacionar pedido e resposta. */

/* Solicita parada alterando apenas a flag sig_atomic_t no tratador de sinal. */
static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}

/* Converte porta decimal e rejeita texto inválido ou valor fora de 1 a 65535. */
static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || port == NULL || *text == '\0')
    {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL || value > UINT16_MAX)
    {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}

/* Converte o texto de --cmd no tipo de mensagem enviado pelo modo de comando. */
static int parse_command_type(const char *text, Message_Type *type)
{
    if (text == NULL || type == NULL)
    {
        return -1;
    }
    if (strcmp(text, "ping") == 0)
    {
        *type = M_PING;
        return 0;
    }
    if (strcmp(text, "join") == 0)
    {
        *type = M_JOIN;
        return 0;
    }
    if (strcmp(text, "leave") == 0)
    {
        *type = M_LEAVE;
        return 0;
    }
    return -1;
}

/* Aceita a forma posicional e as opções longas/curtas processadas por getopt_long. */
static int parse_node_arguments(int argc, char **argv, NodeArguments *arguments)
{
    static const struct option long_options[] = {
        {"cmd", required_argument, NULL, 'c'},
        {"host", required_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {"config", required_argument, NULL, 'f'},
        {"name", required_argument, NULL, 'n'},
        {"chord-host", required_argument, NULL, 'H'},
        {"chord-port", required_argument, NULL, 'P'},
        {NULL, 0, NULL, 0}
    };
    int option;
    int have_command = 0;
    int have_host = 0;
    int have_port = 1;
    int have_name = 0;

    if (arguments == NULL || argc < 1)
    {
        return -1;
    }

    memset(arguments, 0, sizeof(*arguments));
    arguments->node_name = app_config.node_name[0] == '\0' ? "peer" : app_config.node_name;
    arguments->local_port = app_config.port;
    arguments->chord_host = app_config.chord_host[0] == '\0' ? NULL : app_config.chord_host;
    arguments->chord_port = app_config.chord_port;
    if (argc == 1) return 0;

    /* Preserva a interface posicional original: peer <porta> [<ip> <porta>]. */
    if (argv[1][0] != '-')
    {
        if (argc != 2 && argc != 4)
        {
            return -1;
        }
        if (parse_port(argv[1], &arguments->local_port) < 0)
        {
            return -1;
        }
        have_port = 1;
        if (argc == 4)
        {
            arguments->remote_ip = argv[2];
            if (parse_port(argv[3], &arguments->remote_port) < 0)
            {
                return -1;
            }
        }
        return 0;
    }

    opterr = 0;
    optind = 1;
    while ((option = getopt_long(argc, argv, "c:h:p:f:n:H:P:", long_options, NULL)) != -1)
    {
        switch (option)
        {
        case 'c':
            if (parse_command_type(optarg, &arguments->command_type) < 0)
            {
                return -1;
            }
            have_command = 1;
            break;
        case 'h':
            arguments->remote_ip = optarg;
            have_host = optarg[0] != '\0';
            break;
        case 'p':
            if (parse_port(optarg, &arguments->local_port) < 0)
            {
                return -1;
            }
            have_port = 1;
            break;
        case 'f':
            arguments->config_path = optarg;
            break;
        case 'n':
            arguments->node_name = optarg;
            if (arguments->node_name[0] == '\0')
            {
                return -1;
            }
            have_name = 1;
            break;
        case 'H':
            arguments->chord_host = optarg;
            break;
        case 'P':
            if (parse_port(optarg, &arguments->chord_port) < 0) return -1;
            break;
        default:
            return -1;
        }
    }

    if (optind != argc || !have_port || (arguments->chord_host == NULL) != (arguments->chord_port == 0U))
    {
        return -1;
    }

    if (have_command)
    {
        if (!have_host || arguments->config_path != NULL || have_name || arguments->chord_host != NULL)
        {
            return -1;
        }
        arguments->command_mode = 1;
        arguments->remote_port = arguments->local_port;
        arguments->local_port = 0U;
        return 0;
    }

    if (have_host)
    {
        return -1;
    }
    if (arguments->config_path != NULL && access(arguments->config_path, R_OK) != 0)
    {
        return -1;
    }
    return 0;
}

/* Exibe os 32 bytes do NodeID como 64 dígitos hexadecimais. */
static void print_node_id(const uint8_t node_id[NODE_ID_SIZE])
{
    size_t index;

    for (index = 0U; index < NODE_ID_SIZE; ++index)
    {
        printf("%02x", (unsigned)node_id[index]);
    }
}

/* Retorna o nome exibido nos registros TX/RX dos comandos do protocolo. */
static const char *message_type_name(Message_Type type)
{
    switch (type)
    {
    case M_JOIN:
        return "JOIN";
    case M_ACK:
        return "ACK";
    case M_ERROR:
        return "ERROR";
    case M_PING:
        return "PING";
    case M_PONG:
        return "PONG";
    case M_LEAVE:
        return "LEAVE";
    default:
        return "UNKNOWN";
    }
}

/* Aloca e copia um payload textual sem transmitir o terminador NUL. */
static int set_text_payload(Message *message, const char *text)
{
    size_t text_size;

    if (message == NULL || text == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    text_size = strlen(text);
    if (text_size == 0U || text_size > UINT32_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    message->payload = malloc(text_size);
    if (message->payload == NULL)
    {
        return -1;
    }
    memcpy(message->payload, text, text_size);
    message->header.payload_size = (uint32_t)text_size;
    return 0;
}

/* Compara tamanho e bytes do payload com o texto esperado. */
static int message_payload_equals(const Message *message, const char *text)
{
    size_t text_size;

    if (message == NULL || text == NULL)
    {
        return 0;
    }

    text_size = strlen(text);
    return message->header.payload_size == text_size && message->payload != NULL && memcmp(message->payload, text, text_size) == 0;
}

/* Identifica destino zerado, usado no JOIN quando o ID remoto ainda não é conhecido. */
static int node_id_is_zero(const uint8_t node_id[NODE_ID_SIZE])
{
    size_t index;

    for (index = 0U; index < NODE_ID_SIZE; ++index)
    {
        if (node_id[index] != 0U)
        {
            return 0;
        }
    }
    return 1;
}

/*
 * Formato do payload de JOIN na rede (64 bytes):
 *
 *   bytes  0..45: IP textual, incluindo '\0' e preenchimento com zeros
 *   bytes 46..47: porta em ordem de bytes de rede
 *   bytes 48..63: UUID
 *
 * Uma struct Node/NodeConfig não é enviada diretamente porque pode conter
 * preenchimento e representações dependentes da máquina.
 */
/* Serializa IP textual, porta em ordem de rede e UUID no descritor de 64 bytes. */

/* Aloca o descritor de JOIN e transfere sua propriedade ao chamador; libera em caso de erro. */
static int encode_join_payload_alloc(const NodeConfig *config, uint8_t **payload_output)
{
    uint8_t *payload = malloc(JOIN_PAYLOAD_WIRE_SIZE);
    if (payload == NULL) return -1;
    if (rpc_encode_join_payload(config, payload) < 0) { free(payload); return -1; }
    *payload_output = payload;
    return 0;
}

/* Confere tamanho, terminador e preenchimento zero antes de reconstruir a configuração do nó. */

/* Cria identidade local e tabela de Super Peer reutilizando o mesmo UUID e NodeID. */
static int initialize_local_identity(PeerContext *peer, uint16_t local_port)
{
    NodeConfig config;
    SuperPeerConfig superpeer_config;

    char directory[512];
    (void)snprintf(directory, sizeof(directory), ".superpeer_storage/%u", (unsigned)local_port);
    if (app_config.data_dir[0] != '\0') strcpy(directory, app_config.data_dir);
    if (app_identity(directory, &config, local_port) < 0 || node_init(&peer->local_node, &config) < 0)
    {
        return -1;
    }

    peer->local_node.role = NODE_ROLE_SUPERPEER;
    /* Reutiliza o UUID para que Node e SuperPeer locais tenham o mesmo ID. */
    if (superpeer_config_init_with_uuid(&superpeer_config, config.ip, config.port, config.uuid) < 0 || superpeer_create(&superpeer_config, &peer->superpeer) < 0)
    {
        return -1;
    }

    return 0;
}

/* Preserva TransactionID e responde com descritor local ou payload fornecido pelo chamador. */
static int send_reply(const PeerContext *peer, int client_fd, const Message *request, Message_Type type, const uint8_t *payload, uint32_t payload_size, int include_node_descriptor)
{
    Message reply;
    int result;
    uint8_t error_payload[2];

    if (type == M_ERROR) { remote_error_encode(errno, error_payload); payload = error_payload; payload_size = sizeof(error_payload); }

    if (peer == NULL || request == NULL)
    {
        return -1;
    }

    if (message_init(&reply) < 0)
    {
        return -1;
    }
    reply.header.message_type = (uint8_t)type;
    memcpy(reply.header.source_node, peer->local_node.id.bytes, NODE_ID_SIZE);
    memcpy(reply.header.destination_node, request->header.source_node, NODE_ID_SIZE);
    memcpy(reply.header.transaction_id, request->header.transaction_id, TRANSACTION_ID_SIZE);
    reply.header.timestamp = (uint64_t)time(NULL);

    if (include_node_descriptor != 0)
    {
        reply.header.payload_size = JOIN_PAYLOAD_WIRE_SIZE;
        if (encode_join_payload_alloc(&peer->local_node.config, &reply.payload) < 0)
        {
            message_free(&reply);
            return -1;
        }
    }
    else if (payload_size > 0U)
    {
        if (payload == NULL)
        {
            message_free(&reply);
            errno = EINVAL;
            return -1;
        }
        reply.payload = malloc(payload_size);
        if (reply.payload == NULL)
        {
            message_free(&reply);
            return -1;
        }
        memcpy(reply.payload, payload, payload_size);
        reply.header.payload_size = payload_size;
    }
    else
    {
        reply.header.payload_size = 0U;
        reply.payload = NULL;
    }

    result = protocol_send_message(client_fd, &reply);
    message_free(&reply);
    return result;
}

/* Valida destino e identidade recalculada, rejeita o próprio nó e registra antes de permitir ACK. */
static int register_join(PeerContext *peer, const Message *message)
{
    NodeConfig remote_config;
    Node remote_node;
    SuperPeerRegistrationResult registration;

    if (peer == NULL || message == NULL || peer->superpeer == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (!node_id_is_zero(message->header.destination_node) && memcmp(message->header.destination_node, peer->local_node.id.bytes, NODE_ID_SIZE) != 0)
    {
        errno = EHOSTUNREACH;
        return -1;
    }

    uint64_t incarnation = 0U;
    if (message->header.payload_size == JOIN_PAYLOAD_WIRE_SIZE + 9U && message->payload[JOIN_PAYLOAD_WIRE_SIZE] == 1U) incarnation = wire_get_u64(message->payload + JOIN_PAYLOAD_WIRE_SIZE + 1U);
    if ((message->header.payload_size != JOIN_PAYLOAD_WIRE_SIZE && (message->header.payload_size != JOIN_PAYLOAD_WIRE_SIZE + 9U || incarnation == 0U)) || rpc_decode_join_payload(message->payload, JOIN_PAYLOAD_WIRE_SIZE, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0)
    {
        return -1;
    }

    if (memcmp(remote_node.id.bytes, message->header.source_node, NODE_ID_SIZE) != 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (node_id_equal(&remote_node.id, &peer->local_node.id))
    {
        errno = EEXIST;
        return -1;
    }

    registration = superpeer_is_registered(peer->superpeer, &remote_node.id) ? SUPERPEER_MEMBER_UPDATED : SUPERPEER_MEMBER_ADDED;
    if (heartbeat_register(peer->heartbeat, &remote_node, incarnation) < 0)
    {
        return -1;
    }

    /* Mantém a linha inteira do JOIN junta mesmo com atendimentos concorrentes. */
    flockfile(stdout);
    printf("JOIN validado: NodeID=");
    print_node_id(remote_node.id.bytes);
    printf(", resultado=%s, membros=%zu\n", registration == SUPERPEER_MEMBER_ADDED ? "ADDED" : "UPDATED", superpeer_member_count(peer->superpeer));
    funlockfile(stdout);
    return 0;
}

static int register_announcement(PeerContext *peer, const Message *message)
{
    TransferDocument document;
    NodeID owner;

    memcpy(owner.bytes, message->header.source_node, NODE_ID_SIZE);
    heartbeat_gate_lock(peer->heartbeat);
    if (!superpeer_is_registered(peer->superpeer, &owner))
    {
        heartbeat_gate_unlock(peer->heartbeat);
        errno = EACCES;
        return -1;
    }
    MetadataChunk *chunks = NULL;
    if (transfer_decode_announcement(message->payload, message->header.payload_size, &document, &chunks) < 0)
    {
        heartbeat_gate_unlock(peer->heartbeat);
        return -1;
    }
    int status = directory_announce(peer->directory, &document, chunks, &owner);
    free(chunks);
    heartbeat_gate_unlock(peer->heartbeat);
    return status;
}

static int answer_lookup(PeerContext *peer, int client_fd, const Message *message)
{
    TransferSelectorType type;
    TransferLookupResult result;
    ObjectID id;
    char name[METADATA_NAME_SIZE];
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    int status = -1;

    memset(&result, 0, sizeof(result));
    if (transfer_decode_lookup_request(message->payload, message->header.payload_size, &type, &id, name) < 0 || directory_lookup(peer->directory, type, &id, name, &result) < 0 || transfer_encode_lookup_result(&result, &payload, &payload_size) < 0)
    {
        status = send_reply(peer, client_fd, message, M_ERROR, NULL, 0U, 0);
        goto cleanup;
    }
    status = send_reply(peer, client_fd, message, M_DOWNLOAD_REP, payload, payload_size, 0);

cleanup:
    free(payload);
    transfer_lookup_result_free(&result);
    return status;
}

/* JOIN registra; LEAVE remove membro/disponibilidade; C2 usa identidades explícitas. */
static void handle_client(void *context, int client_fd)
{
    PeerContext *peer = context;
    Message message;
    struct sockaddr_in remote_address;
    socklen_t remote_address_len = sizeof(remote_address);
    char source_ip[INET_ADDRSTRLEN] = "desconhecido";

    if (getpeername(client_fd, (struct sockaddr *)&remote_address, &remote_address_len) == 0 && remote_address.sin_family == AF_INET)
    {
        (void)inet_ntop(AF_INET, &remote_address.sin_addr, source_ip, sizeof(source_ip));
    }

    message_init(&message);

    for (;;)
    {
        int result = protocol_receive_message(client_fd, &message);

        if (result == PROTOCOL_CLOSED)
        {
            break;
        }
        if (result != PROTOCOL_OK)
        {
            fprintf(stderr, "Falha ao receber uma mensagem do cliente.\n");
            break;
        }

        /* Evita misturar esta linha com o log de outra conexão. */
        flockfile(stdout);
        printf("Mensagem recebida: tipo=%u", (unsigned)message.header.message_type);
        if (message.header.message_type != (uint8_t)M_PING)
        {
            printf(", origem=");
            print_node_id(message.header.source_node);
        }
        printf(", payload=%" PRIu32 " bytes, ip_origem=%s\n", message.header.payload_size, source_ip);
        funlockfile(stdout);
        fflush(stdout);
        if ((message.header.message_type == M_LOOKUP || message.header.message_type == M_STORE) && (node_id_is_zero(message.header.source_node) || memcmp(message.header.destination_node, peer->local_node.id.bytes, NODE_ID_SIZE) != 0))
        {
            errno = EACCES;
            (void)send_reply(peer, client_fd, &message, M_ERROR, NULL, 0U, 0);
            message_free(&message);
            continue;
        }

        if (message.header.message_type == (uint8_t)M_JOIN)
        {
            Message_Type response_type = register_join(peer, &message) == 0 ? M_ACK : M_ERROR;

            if (response_type == M_ERROR)
            {
                fprintf(stderr, "JOIN rejeitado.\n");
            }
            if (send_reply(peer, client_fd, &message, response_type, NULL, 0U, response_type == M_ACK) < 0)
            {
                fprintf(stderr, "Falha ao enviar resposta ao JOIN.\n");
                message_free(&message);
                break;
            }
        }
        else if (message.header.message_type == (uint8_t)M_PING)
        {
            if (!message_payload_equals(&message, PING_PAYLOAD))
            {
                fprintf(stderr, "PING rejeitado: payload invalido.\n");
                if (send_reply(peer, client_fd, &message, M_ERROR, NULL, 0U, 0) < 0)
                {
                    message_free(&message);
                    break;
                }
            }
            else
            {
                printf("RX PING\n");
                fflush(stdout);
                if (send_reply(peer, client_fd, &message, M_PONG, (const uint8_t *)PONG_PAYLOAD, (uint32_t)(sizeof(PONG_PAYLOAD) - 1U), 0) < 0)
                {
                    fprintf(stderr, "Falha ao enviar PONG.\n");
                    message_free(&message);
                    break;
                }
            }
        }
        else if (message.header.message_type == (uint8_t)M_LEAVE)
        {
            NodeID departed;
            memcpy(departed.bytes, message.header.source_node, NODE_ID_SIZE);
            heartbeat_gate_lock(peer->heartbeat);
            SuperPeerMember leaving;
            int known = superpeer_find_member(peer->superpeer, &departed, &leaving) == 0;
            int valid_leave = message.header.payload_size == 0U ? !known || leaving.incarnation == 0U : message.header.payload_size == 9U && message.payload[0] == 1U && known && wire_get_u64(message.payload + 1U) == leaving.incarnation && memcmp(message.header.destination_node, peer->local_node.id.bytes, NODE_ID_SIZE) == 0;
            if (!valid_leave)
            {
                heartbeat_gate_unlock(peer->heartbeat); errno = ESTALE;
                (void)send_reply(peer, client_fd, &message, M_ERROR, NULL, 0U, 0);
                message_free(&message); message_init(&message); continue;
            }
            if (!node_id_is_zero(departed.bytes) && superpeer_unregister_node(peer->superpeer, &departed) == 0) { (void)metadata_remove_peer(peer->metadata, &departed); (void)heartbeat_checkpoint(peer->heartbeat); }
            heartbeat_gate_unlock(peer->heartbeat);
            if (send_reply(peer, client_fd, &message, M_ACK, NULL, 0U, 0) < 0)
            {
                fprintf(stderr, "Falha ao enviar ACK de LEAVE.\n");
                message_free(&message);
                break;
            }
        }
        else if (message.header.message_type == (uint8_t)M_STORE && message.header.payload_size > 0U && message.payload[0] == TRANSFER_STORE_ANNOUNCE)
        {
            Message_Type response_type = register_announcement(peer, &message) == 0 ? M_ACK : M_ERROR;

            if (send_reply(peer, client_fd, &message, response_type, NULL, 0U, 0) < 0)
            {
                message_free(&message);
                break;
            }
        }
        else if (message.header.message_type == (uint8_t)M_LOOKUP)
        {
            if (answer_lookup(peer, client_fd, &message) < 0)
            {
                message_free(&message);
                break;
            }
        }
        else if (message.header.message_type == M_HEARTBEAT || message.header.message_type == M_GOSSIP)
        {
            uint8_t *payload = NULL; uint32_t size = 0U; Message_Type type = M_ERROR;
            if (heartbeat_handle(peer->heartbeat, &message, &type, &payload, &size) < 0) type = M_ERROR;
            int sent = send_reply(peer, client_fd, &message, type, payload, size, 0);
            free(payload);
            if (sent < 0) { message_free(&message); break; }
        }
        else if (message.header.message_type >= M_CHORD_INFO && message.header.message_type <= M_CHORD_FINGER)
        {
            uint8_t payload[CHORD_ROUTE_WIRE_SIZE];
            uint32_t payload_size = 0U;
            Message_Type response_type = M_ERROR;
            if (message.header.message_type == M_CHORD_NOTIFY && message.header.payload_size == JOIN_PAYLOAD_WIRE_SIZE)
            {
                NodeConfig candidate;
                if (rpc_decode_join_payload(message.payload, message.header.payload_size, &candidate) == 0) (void)heartbeat_discover(peer->heartbeat, &candidate);
            }
            if (chord_network_handle(peer->chord, &message, &response_type, payload, &payload_size) < 0) { response_type = M_ERROR; payload_size = 0U; }
            if (send_reply(peer, client_fd, &message, response_type, payload, payload_size, 0) < 0) { message_free(&message); break; }
        }
        else
        {
            /* Mensagens ainda nao tratadas pelo checkpoint recebem ERROR. */
            errno = ENOTSUP;
            if (send_reply(peer, client_fd, &message, M_ERROR, NULL, 0U, 0) < 0)
            {
                fprintf(stderr, "Falha ao enviar ERROR.\n");
                message_free(&message);
                break;
            }
        }

        message_free(&message);
        message_init(&message);
    }

    message_free(&message);
}

/* Aceita conexões e entrega os descritores ao pool limitado. */
static void *accept_clients(void *argument)
{
    PeerContext *peer = (PeerContext *)argument;

    (void)concurrent_server_run(peer->runtime);
    return NULL;
}

/* Envia JOIN, valida ACK e identidade remota e registra o outro nó na tabela local. */
static int connect_and_join(const PeerContext *peer, const char *ip, uint16_t remote_port)
{
    int socket_fd = network_connect(ip, remote_port);
    Message join;
    Message response;
    NodeConfig remote_config;
    Node remote_node;
    SuperPeerRegistrationResult registration;
    int result;

    if (socket_fd < 0)
    {
        return -1;
    }

    message_init(&join);
    join.header.message_type = (uint8_t)M_JOIN;
    memcpy(join.header.source_node, peer->local_node.id.bytes, NODE_ID_SIZE);
    memset(join.header.destination_node, 0, NODE_ID_SIZE);
    transfer_fill_transaction_id(join.header.transaction_id, join.header.source_node);
    join.header.timestamp = (uint64_t)time(NULL);
    join.header.payload_size = JOIN_PAYLOAD_WIRE_SIZE;

    if (encode_join_payload_alloc(&peer->local_node.config, &join.payload) < 0)
    {
        fprintf(stderr, "Nao foi possivel serializar o payload de JOIN.\n");
        message_free(&join);
        (void)network_shutdown(socket_fd);
        return -1;
    }

    if (protocol_send_message(socket_fd, &join) < 0)
    {
        fprintf(stderr, "Falha ao enviar JOIN.\n");
        message_free(&join);
        (void)network_shutdown(socket_fd);
        return -1;
    }

    message_init(&response);
    result = protocol_receive_message(socket_fd, &response);
    if (result == PROTOCOL_OK)
    {
        if (response.header.message_type != (uint8_t)M_ACK)
        {
            fprintf(stderr, "O peer remoto rejeitou o JOIN (tipo=%u).\n", (unsigned)response.header.message_type);
            result = PROTOCOL_ERROR;
        }
        else if (memcmp(response.header.transaction_id, join.header.transaction_id, TRANSACTION_ID_SIZE) != 0 || memcmp(response.header.destination_node, peer->local_node.id.bytes, NODE_ID_SIZE) != 0)
        {
            fprintf(stderr, "A resposta possui identificadores diferentes.\n");
            result = PROTOCOL_ERROR;
        }
        else if (rpc_decode_join_payload(response.payload, response.header.payload_size, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0 || memcmp(remote_node.id.bytes, response.header.source_node, NODE_ID_SIZE) != 0 || node_id_equal(&remote_node.id, &peer->local_node.id))
        {
            fprintf(stderr, "A identidade do peer remoto e invalida.\n");
            result = PROTOCOL_ERROR;
        }
        else if ((registration = superpeer_register_node(peer->superpeer, &remote_node)) == SUPERPEER_REGISTER_ERROR)
        {
            fprintf(stderr, "Nao foi possivel registrar o peer remoto.\n");
            result = PROTOCOL_ERROR;
        }
        else
        {
            printf("JOIN aceito pelo peer remoto: tipo=%u, membro=%s, " "membros=%zu\n", (unsigned)response.header.message_type, registration == SUPERPEER_MEMBER_ADDED ? "ADDED" : "UPDATED", superpeer_member_count(peer->superpeer));
        }
    }
    else if (result == PROTOCOL_CLOSED)
    {
        fprintf(stderr, "O peer remoto fechou a conexao sem responder.\n");
    }
    else
    {
        fprintf(stderr, "Falha ao receber a resposta do JOIN.\n");
    }

    message_free(&join);
    message_free(&response);
    (void)network_shutdown(socket_fd);
    return result == PROTOCOL_OK ? 0 : -1;
}

/* Executa PING, JOIN ou LEAVE uma vez e encerra, usando o mesmo binário do peer. */
static int execute_command(const NodeArguments *arguments)
{
    int socket_fd;
    int result = -1;
    Message request;
    Message response;
    Message_Type expected_type;
    NodeConfig config;
    Node node;

    if (arguments == NULL || !arguments->command_mode || arguments->remote_ip == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    socket_fd = network_connect(arguments->remote_ip, arguments->remote_port);
    if (socket_fd < 0)
    {
        return -1;
    }

    if (message_init(&request) != PROTOCOL_OK || message_init(&response) != PROTOCOL_OK)
    {
        (void)network_shutdown(socket_fd);
        return -1;
    }

    request.header.message_type = (uint8_t)arguments->command_type;
    if (arguments->command_type == M_PING)
    {
        expected_type = M_PONG;
        if (set_text_payload(&request, PING_PAYLOAD) < 0)
        {
            goto cleanup;
        }
    }
    else if (arguments->command_type == M_JOIN)
    {
        expected_type = M_ACK;
        if (node_config_init(&config, DEFAULT_LOCAL_IP, 1U) < 0 || node_init(&node, &config) < 0 || encode_join_payload_alloc(&config, &request.payload) < 0)
        {
            goto cleanup;
        }
        request.header.payload_size = JOIN_PAYLOAD_WIRE_SIZE;
        memcpy(request.header.source_node, node.id.bytes, NODE_ID_SIZE);
    }
    else if (arguments->command_type == M_LEAVE)
    {
        expected_type = M_ACK;
    }
    else
    {
        errno = EINVAL;
        goto cleanup;
    }

    transfer_fill_transaction_id(request.header.transaction_id, request.header.source_node);
    request.header.timestamp = (uint64_t)time(NULL);
    printf("TX %s\n", message_type_name(arguments->command_type));
    fflush(stdout);

    if (protocol_send_message(socket_fd, &request) != PROTOCOL_OK)
    {
        fprintf(stderr, "Falha ao enviar %s.\n", message_type_name(arguments->command_type));
        goto cleanup;
    }

    if (protocol_receive_message(socket_fd, &response) != PROTOCOL_OK)
    {
        fprintf(stderr, "Falha ao receber a resposta.\n");
        goto cleanup;
    }
    if (response.header.message_type != (uint8_t)expected_type || memcmp(response.header.transaction_id, request.header.transaction_id, TRANSACTION_ID_SIZE) != 0)
    {
        fprintf(stderr, "Resposta invalida: esperado %s, recebido %s.\n", message_type_name(expected_type), message_type_name((Message_Type)response.header.message_type));
        goto cleanup;
    }
    if (expected_type == M_PONG && !message_payload_equals(&response, PONG_PAYLOAD))
    {
        fprintf(stderr, "PONG recebido com payload invalido.\n");
        goto cleanup;
    }

    printf("RX %s\n", message_type_name(expected_type));
    fflush(stdout);
    result = 0;

cleanup:
    message_free(&request);
    message_free(&response);
    (void)network_shutdown(socket_fd);
    return result;
}

/* Compõe 16 bytes com horário, PID e sequência local para correlacionar pedido e resposta. */

/* Mostra os modos servidor, conexão entre peers e comando de teste. */
static void print_usage(const char *program_name)
{
    fprintf(stderr, "Uso: %s <porta-local> [<ip-remoto> <porta-remota>]\n" "   ou: %s --port <porta> [--chord-host <ip> --chord-port <porta>]\n" "   ou: %s --cmd <ping|join|leave> --host <ip> --port <porta>\n", program_name, program_name, program_name);
}

/* Efeitos locais ordenados pelo gate do detector; não há RPC nestes callbacks. */
static void membership_failure(void *context, const SuperPeerMember *member)
{
    PeerContext *peer = context;
    if (member->node.role == NODE_ROLE_SUPERPEER) (void)chord_forget(peer->chord, &member->node.id);
    else if (member->local_registration) (void)metadata_remove_peer(peer->metadata, &member->node.id);
}

static void refresh_chord_membership(PeerContext *peer)
{
    ChordPeer neighbor;
    if (chord_get_successor(peer->chord, &neighbor) == 0) (void)heartbeat_discover(peer->heartbeat, &neighbor.config);
    int exists;
    if (chord_get_predecessor(peer->chord, &neighbor, &exists) == 0 && exists) (void)heartbeat_discover(peer->heartbeat, &neighbor.config);
    for (unsigned i = 0U; i < CHORD_FINGER_COUNT; ++i) if (chord_get_finger(peer->chord, i, &neighbor) == 0) (void)heartbeat_discover(peer->heartbeat, &neighbor.config);
    SuperPeerMember *members; size_t count;
    heartbeat_gate_lock(peer->heartbeat);
    if (membership_snapshot(peer->superpeer, &members, &count) == 0)
    {
        ChordPeer *alive = calloc(count, sizeof(*alive)); size_t n = 0U;
        if (alive != NULL)
        {
            int successor_unavailable = 0;
            ChordPeer current_successor;
            (void)chord_get_successor(peer->chord, &current_successor);
            for (size_t i = 0U; i < count; ++i)
            {
                if (members[i].node.role != NODE_ROLE_SUPERPEER || node_id_equal(&members[i].node.id, &peer->local_node.id)) continue;
                if (members[i].state >= SUPERPEER_MEMBER_FAILED) (void)chord_forget(peer->chord, &members[i].node.id);
                if (node_id_equal(&members[i].node.id, &current_successor.id) && members[i].state != SUPERPEER_MEMBER_ALIVE) successor_unavailable = 1;
                if (members[i].state == SUPERPEER_MEMBER_ALIVE && members[i].incarnation > 0U && members[i].last_seen != 0)
                {
                    (void)chord_allow(peer->chord, &members[i].node.id);
                    alive[n++] = (ChordPeer){members[i].node.id, members[i].node.config};
                }
            }
            /* Antes da descoberta direta, mantém o sucessor aprendido no JOIN Chord. */
            if (n > 0U || successor_unavailable) (void)chord_repair(peer->chord, alive, n);
            free(alive);
        }
        free(members);
    }
    heartbeat_gate_unlock(peer->heartbeat);
}

/* Inicializa identidade, sincronização e servidor; no encerramento espera clientes antes de destruir o estado. */
static int superpeer_run(int argc, char **argv)
{
    /* O roteiro do professor mistura stdout de processos no mesmo arquivo: nunca sobrescrever linhas já anexadas. */
    struct stat log_stat;
    if (fstat(STDOUT_FILENO, &log_stat) == 0 && S_ISREG(log_stat.st_mode))
    {
        int flags = fcntl(STDOUT_FILENO, F_GETFL);
        if (flags >= 0) (void)fcntl(STDOUT_FILENO, F_SETFL, flags | O_APPEND);
    }
    NodeArguments arguments;
    PeerContext peer;
    pthread_t accept_thread;
    int thread_result;

    // Inicializa configuração e interpreta argumentos; --cmd ignora --config e --name.
    if (app_config_load(&argc, argv, 1) < 0 || parse_node_arguments(argc, argv, &arguments) < 0)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    (void)signal(SIGPIPE, SIG_IGN);
    // Se o modo de comando foi solicitado, executa e encerra imediatamente.
    if (arguments.command_mode)
    {
        return execute_command(&arguments) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // Configura tratadores de sinal para encerrar o peer com CTRL+C ou kill.
    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);

    // Inicializa identidade local, tabela de membros e diretório de metadados; encerra em caso de falha.
    memset(&peer, 0, sizeof(peer));
    peer.server_fd = -1;
    if (initialize_local_identity(&peer, arguments.local_port) < 0)
    {
        fprintf(stderr, "Nao foi possivel inicializar a identidade local.\n");
        return EXIT_FAILURE;
    }
    if (metadata_create(&peer.metadata) < 0 || directory_create(peer.metadata, peer.superpeer, &peer.directory) < 0 || chord_create(&peer.local_node, &peer.chord) < 0)
    {
        fprintf(stderr, "Nao foi possivel inicializar o diretorio de metadados.\n");
        metadata_destroy(peer.metadata);
        chord_destroy(peer.chord);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    char membership_directory[512];
    (void)snprintf(membership_directory, sizeof(membership_directory), ".superpeer_storage/%u", (unsigned)arguments.local_port);
    if (app_config.data_dir[0] != '\0') strcpy(membership_directory, app_config.data_dir);
    if (heartbeat_create(&peer.local_node, peer.superpeer, membership_directory, membership_failure, &peer, &peer.heartbeat) < 0)
    {
        perror("membership initialization"); directory_destroy(peer.directory); metadata_destroy(peer.metadata); chord_destroy(peer.chord); superpeer_destroy(peer.superpeer); return EXIT_FAILURE;
    }
    peer.server_fd = network_create_server(arguments.local_port, PEER_BACKLOG);
    if (peer.server_fd < 0 || concurrent_server_create(peer.server_fd, handle_client, &peer, &peer.runtime) < 0)
    {
        if (peer.server_fd >= 0)
        {
            (void)network_shutdown(peer.server_fd);
        }
        directory_destroy(peer.directory);
        heartbeat_destroy(peer.heartbeat);
        metadata_destroy(peer.metadata);
        chord_destroy(peer.chord);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    printf("Node %s started\n", arguments.node_name);
    printf("NodeID: ");
    print_node_id(peer.local_node.id.bytes);
    printf("\n");
    printf("Peer ouvindo na porta %" PRIu16 ", NodeID=", arguments.local_port);
    print_node_id(peer.local_node.id.bytes);
    printf(", membros locais=%zu\n", superpeer_member_count(peer.superpeer));
    fflush(stdout);

    thread_result = pthread_create(&accept_thread, NULL, accept_clients, &peer);
    if (thread_result != 0)
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(thread_result));
        concurrent_server_destroy(peer.runtime);
        directory_destroy(peer.directory);
        heartbeat_destroy(peer.heartbeat);
        metadata_destroy(peer.metadata);
        chord_destroy(peer.chord);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    if (arguments.remote_ip != NULL && connect_and_join(&peer, arguments.remote_ip, arguments.remote_port) < 0)
    {
        fprintf(stderr, "Nao foi possivel concluir o JOIN remoto.\n");
    }

    /* O inventário permite iniciar processos em paralelo enquanto o bootstrap sobe. */
    int chord_joined = arguments.chord_host == NULL;
    for (unsigned attempt = 0U; arguments.chord_host != NULL && attempt < 30U && g_running; ++attempt)
    {
        if (chord_network_join(peer.chord, arguments.chord_host, arguments.chord_port) == 0) { chord_joined = 1; break; }
        if (attempt == 0U) fprintf(stderr, "Aguardando bootstrap Chord %s:%u.\n", arguments.chord_host, (unsigned)arguments.chord_port);
        (void)sleep(1U);
    }
    if (!chord_joined) { fprintf(stderr, "Nao foi possivel entrar no anel Chord.\n"); g_running = 0; }

    if (heartbeat_start(peer.heartbeat) < 0) { perror("heartbeat start"); g_running = 0; }
    else { printf("GOSSIP/heartbeat ativo\n"); fflush(stdout); }

    /* Mantem o processo vivo para aceitar novos clientes. */
    unsigned finger_index = 0U;
    while (g_running)
    {
        refresh_chord_membership(&peer);
        int64_t previous_deadline = network_deadline_set(network_monotonic_ms() + heartbeat_budget(peer.heartbeat));
        for (unsigned count = 0U; count < 16U && g_running; ++count)
        {
            (void)chord_network_maintain(peer.chord, finger_index);
            finger_index = (finger_index + 1U) % CHORD_FINGER_COUNT;
        }
        network_deadline_set(previous_deadline);
        (void)sleep(1U);
    }

    /* Interrompe novas conexões antes de encerrar clientes e liberar o estado compartilhado. */
    concurrent_server_stop(peer.runtime);
    (void)pthread_join(accept_thread, NULL);
    concurrent_server_destroy(peer.runtime);
    heartbeat_destroy(peer.heartbeat);
    directory_destroy(peer.directory);
    metadata_destroy(peer.metadata);
    chord_destroy(peer.chord);
    superpeer_destroy(peer.superpeer);
    printf("Peer encerrado.\n");
    return EXIT_SUCCESS;
}

/* Ponto de entrada do Super Peer, após a API de membros e o atendimento TCP. */
int main(int argc, char **argv)
{
    return superpeer_run(argc, argv);
}
