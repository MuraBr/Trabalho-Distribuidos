#include "network.h"
#include "node.h"
#include "protocol.h"
#include "superpeer.h"

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

#define PEER_BACKLOG 10
#define DEFAULT_LOCAL_IP "127.0.0.1"
#define PING_PAYLOAD "PING"
#define PONG_PAYLOAD "PONG"

typedef struct PeerContext PeerContext;

/* Conexão atendida por uma thread e encadeada na lista de clientes ativos. */
typedef struct ClientContext
{
    PeerContext *peer;
    int client_fd;
    struct ClientContext *next;
} ClientContext;

/* Estado compartilhado: identidade, cadastro de membros e sincronização das conexões. */
struct PeerContext
{
    int server_fd;
    Node local_node;
    SuperPeer *superpeer;
    pthread_mutex_t clients_mutex;
    pthread_cond_t clients_cond;
    ClientContext *clients;
};

typedef struct
{
    uint16_t local_port;
    const char *remote_ip;
    uint16_t remote_port;
    const char *config_path;
    const char *node_name;
    Message_Type command_type;
    int command_mode;
} NodeArguments; /* Opções de inicialização; config_path ainda não tem conteúdo interpretado. */

static volatile sig_atomic_t g_running = 1;

/* Compõe 16 bytes com horário, PID e sequência local para correlacionar pedido e resposta. */
static void fill_transaction_id(uint8_t transaction_id[TRANSACTION_ID_SIZE]);

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
        {NULL, 0, NULL, 0}
    };
    int option;
    int have_command = 0;
    int have_host = 0;
    int have_port = 0;
    int have_name = 0;

    if (arguments == NULL || argc < 2)
    {
        return -1;
    }

    memset(arguments, 0, sizeof(*arguments));
    arguments->node_name = "peer";

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
    while ((option = getopt_long(argc, argv, "c:h:p:f:n:", long_options, NULL)) != -1)
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
        default:
            return -1;
        }
    }

    if (optind != argc || !have_port)
    {
        return -1;
    }

    if (have_command)
    {
        if (!have_host || arguments->config_path != NULL || have_name)
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
static int encode_join_payload(const NodeConfig *config, uint8_t *payload, size_t payload_size)
{
    uint16_t network_port;

    if (config == NULL || payload == NULL || payload_size != JOIN_PAYLOAD_WIRE_SIZE || node_config_validate(config) < 0)
    {
        return -1;
    }

    memset(payload, 0, payload_size);
    memcpy(payload, config->ip, NODE_ADDRESS_SIZE);

    network_port = htons(config->port);
    memcpy(payload + NODE_ADDRESS_SIZE, &network_port, sizeof(network_port));
    memcpy(payload + NODE_ADDRESS_SIZE + sizeof(network_port), config->uuid, NODE_UUID_SIZE);
    return 0;
}

/* Aloca o descritor de JOIN e transfere sua propriedade ao chamador; libera em caso de erro. */
static int encode_join_payload_alloc(const NodeConfig *config, uint8_t **payload_output)
{
    uint8_t *payload;

    if (payload_output == NULL)
    {
        return -1;
    }
    *payload_output = NULL;

    payload = malloc(JOIN_PAYLOAD_WIRE_SIZE);
    if (payload == NULL)
    {
        return -1;
    }
    if (encode_join_payload(config, payload, JOIN_PAYLOAD_WIRE_SIZE) < 0)
    {
        free(payload);
        return -1;
    }

    *payload_output = payload;
    return 0;
}

/* Confere tamanho, terminador e preenchimento zero antes de reconstruir a configuração do nó. */
static int decode_join_payload(const uint8_t *payload, size_t payload_size, NodeConfig *config)
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

/* Cria identidade local e tabela de Super Peer reutilizando o mesmo UUID e NodeID. */
static int initialize_local_identity(PeerContext *peer, uint16_t local_port)
{
    NodeConfig config;
    SuperPeerConfig superpeer_config;

    if (node_config_init(&config, DEFAULT_LOCAL_IP, local_port) < 0 || node_init(&peer->local_node, &config) < 0)
    {
        return -1;
    }

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

    if (decode_join_payload(message->payload, message->header.payload_size, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0)
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

    registration = superpeer_register_node(peer->superpeer, &remote_node);
    if (registration == SUPERPEER_REGISTER_ERROR)
    {
        return -1;
    }

    printf("JOIN validado: NodeID=");
    print_node_id(remote_node.id.bytes);
    printf(", resultado=%s, membros=%zu\n", registration == SUPERPEER_MEMBER_ADDED ? "ADDED" : "UPDATED", superpeer_member_count(peer->superpeer));
    return 0;
}

/* Insere a conexão na lista protegida pelo mutex para acompanhar seu ciclo de vida. */
static void track_client(PeerContext *peer, ClientContext *client)
{
    (void)pthread_mutex_lock(&peer->clients_mutex);
    client->next = peer->clients;
    peer->clients = client;
    (void)pthread_mutex_unlock(&peer->clients_mutex);
}

/* Remove a conexão sob mutex e sinaliza quem espera o esvaziamento da lista. */
static void untrack_client(ClientContext *client)
{
    PeerContext *peer = client->peer;
    ClientContext **current;

    (void)pthread_mutex_lock(&peer->clients_mutex);
    current = &peer->clients;
    while (*current != NULL && *current != client)
    {
        current = &(*current)->next;
    }
    if (*current == client)
    {
        *current = client->next;
    }
    (void)pthread_cond_broadcast(&peer->clients_cond);
    (void)pthread_mutex_unlock(&peer->clients_mutex);
}

/* Fecha os sockets acompanhados sob mutex e marca os descritores como indisponíveis. */
static void close_tracked_clients(PeerContext *peer)
{
    ClientContext *client;

    (void)pthread_mutex_lock(&peer->clients_mutex);
    for (client = peer->clients; client != NULL; client = client->next)
    {
        if (client->client_fd >= 0)
        {
            int client_fd = client->client_fd;

            client->client_fd = -1;
            (void)network_shutdown(client_fd);
        }
    }
    (void)pthread_mutex_unlock(&peer->clients_mutex);
}

/* Espera a lista esvaziar; pthread_cond_wait libera o mutex enquanto aguarda e o readquire ao retornar. */
static void wait_for_clients(PeerContext *peer)
{
    (void)pthread_mutex_lock(&peer->clients_mutex);
    while (peer->clients != NULL)
    {
        (void)pthread_cond_wait(&peer->clients_cond, &peer->clients_mutex);
    }
    (void)pthread_mutex_unlock(&peer->clients_mutex);
}

/* Processa mensagens da conexão: JOIN registra, PING recebe PONG e LEAVE apenas recebe ACK. */
static void *handle_client(void *argument)
{
    ClientContext *client = (ClientContext *)argument;
    PeerContext *peer = client->peer;
    int client_fd = client->client_fd;
    Message message;

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

        printf("Mensagem recebida: tipo=%u, origem=", (unsigned)message.header.message_type);
        print_node_id(message.header.source_node);
        printf(", payload=%" PRIu32 " bytes\n", message.header.payload_size);

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
            if (send_reply(peer, client_fd, &message, M_ACK, NULL, 0U, 0) < 0)
            {
                fprintf(stderr, "Falha ao enviar ACK de LEAVE.\n");
                message_free(&message);
                break;
            }
        }
        else
        {
            /* Mensagens ainda nao tratadas pelo checkpoint recebem ERROR. */
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
    if (client_fd >= 0)
    {
        int owned_fd = -1;

        (void)pthread_mutex_lock(&peer->clients_mutex);
        if (client->client_fd == client_fd)
        {
            client->client_fd = -1;
            owned_fd = client_fd;
        }
        (void)pthread_mutex_unlock(&peer->clients_mutex);
        if (owned_fd >= 0)
        {
            (void)network_shutdown(owned_fd);
        }
    }
    untrack_client(client);
    free(client);
    return NULL;
}

/* Aceita conexões e cria uma thread destacada por cliente; desfaz o cadastro se a criação falhar. */
static void *accept_clients(void *argument)
{
    PeerContext *peer = (PeerContext *)argument;

    while (g_running)
    {
        int client_fd = network_accept_client(peer->server_fd);

        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (g_running)
            {
                fprintf(stderr, "Nao foi possivel aceitar um cliente.\n");
            }
            break;
        }

        ClientContext *client = calloc(1U, sizeof(*client));
        if (client == NULL)
        {
            perror("calloc");
            (void)network_shutdown(client_fd);
            continue;
        }

        client->peer = peer;
        client->client_fd = client_fd;
        track_client(peer, client);

        pthread_t thread;
        int thread_result = pthread_create(&thread, NULL, handle_client, client);
        if (thread_result != 0)
        {
            fprintf(stderr, "pthread_create: %s\n", strerror(thread_result));
            untrack_client(client);
            free(client);
            (void)network_shutdown(client_fd);
            continue;
        }

        (void)pthread_detach(thread);
    }

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
    fill_transaction_id(join.header.transaction_id);
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
        else if (decode_join_payload(response.payload, response.header.payload_size, &remote_config) < 0 || node_init(&remote_node, &remote_config) < 0 || memcmp(remote_node.id.bytes, response.header.source_node, NODE_ID_SIZE) != 0 || node_id_equal(&remote_node.id, &peer->local_node.id))
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

    fill_transaction_id(request.header.transaction_id);
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
static void fill_transaction_id(uint8_t transaction_id[TRANSACTION_ID_SIZE])
{
    static uint32_t sequence = 0U;
    uint64_t timestamp = (uint64_t)time(NULL);
    uint32_t process_value = htonl((uint32_t)getpid());
    uint32_t sequence_value = htonl(++sequence);
    size_t index;

    /* O TransactionID e tratado como uma sequencia opaca de 16 bytes. */
    for (index = 0U; index < sizeof(timestamp); ++index)
    {
        size_t shift = 56U - (index * 8U);

        transaction_id[index] = (uint8_t)(timestamp >> shift);
    }
    memcpy(transaction_id + 8U, &process_value, sizeof(process_value));
    memcpy(transaction_id + 12U, &sequence_value, sizeof(sequence_value));
}

/* Mostra os modos servidor, conexão entre peers e comando de teste. */
static void print_usage(const char *program_name)
{
    fprintf(stderr, "Uso: %s <porta-local> [<ip-remoto> <porta-remota>]\n" "   ou: %s --config <arquivo> --port <porta> --name <nome>\n" "   ou: %s --cmd <ping|join|leave> --host <ip> --port <porta>\n", program_name, program_name, program_name);
}

/* Inicializa identidade, sincronização e servidor; no encerramento espera clientes antes de destruir o estado. */
int main(int argc, char **argv)
{
    NodeArguments arguments;
    PeerContext peer;
    pthread_t accept_thread;
    int thread_result;
    int mutex_result;
    int condition_result;

    if (parse_node_arguments(argc, argv, &arguments) < 0)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    (void)signal(SIGPIPE, SIG_IGN);

    if (arguments.command_mode)
    {
        return execute_command(&arguments) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);

    memset(&peer, 0, sizeof(peer));
    peer.server_fd = -1;
    if (initialize_local_identity(&peer, arguments.local_port) < 0)
    {
        fprintf(stderr, "Nao foi possivel inicializar a identidade local.\n");
        return EXIT_FAILURE;
    }

    mutex_result = pthread_mutex_init(&peer.clients_mutex, NULL);
    if (mutex_result != 0)
    {
        fprintf(stderr, "pthread_mutex_init: %s\n", strerror(mutex_result));
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }
    condition_result = pthread_cond_init(&peer.clients_cond, NULL);
    if (condition_result != 0)
    {
        fprintf(stderr, "pthread_cond_init: %s\n", strerror(condition_result));
        (void)pthread_mutex_destroy(&peer.clients_mutex);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    peer.server_fd = network_create_server(arguments.local_port, PEER_BACKLOG);
    if (peer.server_fd < 0)
    {
        (void)pthread_cond_destroy(&peer.clients_cond);
        (void)pthread_mutex_destroy(&peer.clients_mutex);
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
        (void)network_shutdown(peer.server_fd);
        (void)pthread_cond_destroy(&peer.clients_cond);
        (void)pthread_mutex_destroy(&peer.clients_mutex);
        superpeer_destroy(peer.superpeer);
        return EXIT_FAILURE;
    }

    if (arguments.remote_ip != NULL && connect_and_join(&peer, arguments.remote_ip, arguments.remote_port) < 0)
    {
        fprintf(stderr, "Nao foi possivel concluir o JOIN remoto.\n");
    }

    /* Mantem o processo vivo para aceitar novos clientes. */
    while (g_running)
    {
        (void)sleep(1U);
    }

    /* Interrompe novas conexões antes de encerrar clientes e liberar o estado compartilhado. */
    (void)network_shutdown(peer.server_fd);
    (void)pthread_join(accept_thread, NULL);
    close_tracked_clients(&peer);
    wait_for_clients(&peer);
    (void)pthread_cond_destroy(&peer.clients_cond);
    (void)pthread_mutex_destroy(&peer.clients_mutex);
    superpeer_destroy(peer.superpeer);
    printf("Peer encerrado.\n");
    return EXIT_SUCCESS;
}
