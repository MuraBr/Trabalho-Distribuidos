#include "network.h"
#include "node.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Operações disponíveis no cliente de teste do checkpoint 1. */
typedef enum
{
    CLIENT_PING,
    CLIENT_JOIN,
    CLIENT_LEAVE
} ClientCommand;

typedef struct
{
    ClientCommand command;
    const char *host;
    uint16_t port;
} ClientArguments; /* Comando e endereço do servidor fornecidos na linha de comando. */

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

/* Traduz ping, join e leave para o comando interno; rejeita outros nomes. */
static int parse_command(const char *text, ClientCommand *command)
{
    if (text == NULL || command == NULL)
    {
        return -1;
    }
    if (strcmp(text, "ping") == 0)
    {
        *command = CLIENT_PING;
        return 0;
    }
    if (strcmp(text, "join") == 0)
    {
        *command = CLIENT_JOIN;
        return 0;
    }
    if (strcmp(text, "leave") == 0)
    {
        *command = CLIENT_LEAVE;
        return 0;
    }
    return -1;
}

/* Exige comando, host e porta, cada um acompanhado de seu valor na linha de comando. */
static int parse_arguments(int argc, char **argv, ClientArguments *arguments)
{
    int index;
    int have_command = 0;
    int have_host = 0;
    int have_port = 0;

    if (arguments == NULL)
    {
        return -1;
    }

    memset(arguments, 0, sizeof(*arguments));
    for (index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--cmd") == 0 && index + 1 < argc)
        {
            if (parse_command(argv[++index], &arguments->command) < 0)
            {
                return -1;
            }
            have_command = 1;
        }
        else if (strcmp(argv[index], "--host") == 0 && index + 1 < argc)
        {
            arguments->host = argv[++index];
            have_host = arguments->host[0] != '\0';
        }
        else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc)
        {
            if (parse_port(argv[++index], &arguments->port) < 0)
            {
                return -1;
            }
            have_port = 1;
        }
        else
        {
            return -1;
        }
    }

    return have_command && have_host && have_port ? 0 : -1;
}

/* Compõe 16 bytes com horário, PID e sequência local para correlacionar pedido e resposta. */
static void fill_transaction_id(uint8_t transaction_id[TRANSACTION_ID_SIZE])
{
    static uint32_t sequence = 0U;
    uint64_t timestamp = (uint64_t)time(NULL);
    uint32_t process_value = htonl((uint32_t)getpid());
    uint32_t sequence_value = htonl(++sequence);
    size_t index;

    for (index = 0U; index < sizeof(timestamp); ++index)
    {
        size_t shift = 56U - (index * 8U);

        transaction_id[index] = (uint8_t)(timestamp >> shift);
    }
    memcpy(transaction_id + 8U, &process_value, sizeof(process_value));
    memcpy(transaction_id + 12U, &sequence_value, sizeof(sequence_value));
}

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

/* Mostra os argumentos aceitos pelo cliente de teste. */
static void print_usage(const char *program_name)
{
    fprintf(stderr, "Uso: %s --cmd <ping|join|leave> --host <ip> --port <porta>\n", program_name);
}

/* Conecta, envia um comando e confere tipo e TransactionID da resposta; libera mensagens e socket. */
int main(int argc, char **argv)
{
    ClientArguments arguments;
    Message request;
    Message response;
    Node node;
    NodeConfig config;
    int socket_fd;
    int result;
    Message_Type expected_type;

    if (parse_arguments(argc, argv, &arguments) < 0)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    socket_fd = network_connect(arguments.host, arguments.port);
    if (socket_fd < 0)
    {
        return EXIT_FAILURE;
    }

    (void)signal(SIGPIPE, SIG_IGN);
    if (message_init(&request) < 0)
    {
        (void)network_shutdown(socket_fd);
        return EXIT_FAILURE;
    }

    if (arguments.command == CLIENT_PING)
    {
        request.header.message_type = (uint8_t)M_PING;
        expected_type = M_PONG;
    }
    else if (arguments.command == CLIENT_JOIN)
    {
        /* Descritor de teste: porta 1 não representa um servidor aberto por este cliente. */
        request.header.message_type = (uint8_t)M_JOIN;
        expected_type = M_ACK;
        if (node_config_init(&config, "127.0.0.1", 1U) < 0 || node_init(&node, &config) < 0)
        {
            message_free(&request);
            (void)network_shutdown(socket_fd);
            return EXIT_FAILURE;
        }
        request.header.payload_size = JOIN_PAYLOAD_WIRE_SIZE;
        request.payload = malloc(JOIN_PAYLOAD_WIRE_SIZE);
        if (request.payload == NULL || encode_join_payload(&config, request.payload, JOIN_PAYLOAD_WIRE_SIZE) < 0)
        {
            message_free(&request);
            (void)network_shutdown(socket_fd);
            return EXIT_FAILURE;
        }
        memcpy(request.header.source_node, node.id.bytes, NODE_ID_SIZE);
    }
    else
    {
        request.header.message_type = (uint8_t)M_LEAVE;
        expected_type = M_ACK;
    }

    fill_transaction_id(request.header.transaction_id);
    request.header.timestamp = (uint64_t)time(NULL);
    if (protocol_send_message(socket_fd, &request) < 0)
    {
        message_free(&request);
        (void)network_shutdown(socket_fd);
        return EXIT_FAILURE;
    }

    message_init(&response);
    result = protocol_receive_message(socket_fd, &response);
    if (result != PROTOCOL_OK || response.header.message_type != (uint8_t)expected_type || memcmp(response.header.transaction_id, request.header.transaction_id, TRANSACTION_ID_SIZE) != 0)
    {
        fprintf(stderr, "Resposta invalida do servidor.\n");
        message_free(&request);
        message_free(&response);
        (void)network_shutdown(socket_fd);
        return EXIT_FAILURE;
    }

    if (expected_type == M_PONG)
    {
        printf("RX PONG\n");
    }
    else
    {
        printf("RX ACK\n");
    }
    fflush(stdout);

    message_free(&request);
    message_free(&response);
    (void)network_shutdown(socket_fd);
    return EXIT_SUCCESS;
}
