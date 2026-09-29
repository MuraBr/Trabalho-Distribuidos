/* Ponto de entrada do Peer de armazenamento e dos comandos do cliente. */
#include "file_client.h"
#include "app_config.h"
#include "local_control.h"
#include "node.h"
#include "peer_service.h"
#include "rpc.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SUPERPEER_HOST "127.0.0.1"
#define DEFAULT_SUPERPEER_PORT 55101U
#define DEFAULT_PEER_HOST "127.0.0.1"
#define DEFAULT_PEER_PORT 55102U
static uint16_t local_peer_port = DEFAULT_PEER_PORT;

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || port == NULL || text[0] == '\0')
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

static const char *message_name(Message_Type type)
{
    switch (type)
    {
    case M_JOIN:
        return "JOIN";
    case M_LEAVE:
        return "LEAVE";
    case M_PING:
        return "PING";
    case M_PONG:
        return "PONG";
    case M_ACK:
        return "ACK";
    default:
        return "ERROR";
    }
}

static int legacy_command(const char *command, const char *host, uint16_t port)
{
    Message_Type request_type;
    Message_Type expected_type;
    const uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    uint8_t join_payload[JOIN_PAYLOAD_WIRE_SIZE];
    Node local_node;
    NodeConfig config;
    const NodeID *source = NULL;
    Message response;
    int status = -1;

    if (strcmp(command, "ping") == 0)
    {
        request_type = M_PING;
        expected_type = M_PONG;
        payload = (const uint8_t *)"PING";
        payload_size = 4U;
    }
    else if (strcmp(command, "join") == 0)
    {
        request_type = M_JOIN;
        expected_type = M_ACK;
        if (node_config_init(&config, "127.0.0.1", 65534U) < 0 || node_init(&local_node, &config) < 0 || rpc_encode_join_payload(&config, join_payload) < 0)
        {
            return -1;
        }
        source = &local_node.id;
        payload = join_payload;
        payload_size = JOIN_PAYLOAD_WIRE_SIZE;
    }
    else if (strcmp(command, "leave") == 0)
    {
        request_type = M_LEAVE;
        expected_type = M_ACK;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }
    printf("TX %s\n", message_name(request_type));
    if (rpc_call(host, port, source, NULL, request_type, payload, payload_size, &response) < 0)
    {
        return -1;
    }
    if (response.header.message_type == (uint8_t)expected_type && (expected_type != M_PONG || (response.header.payload_size == 4U && memcmp(response.payload, "PONG", 4U) == 0)))
    {
        printf("RX %s\n", message_name(expected_type));
        status = 0;
    }
    else
    {
        errno = EREMOTEIO;
    }
    message_free(&response);
    return status;
}

static int option_command(int argc, char **argv)
{
    const char *command = NULL;
    const char *host = NULL;
    const char *file = NULL;
    const char *output = NULL;
    uint16_t port = 0U;
    int index;

    for (index = 1; index < argc; index += 2)
    {
        if (index + 1 >= argc)
        {
            errno = EINVAL;
            return -1;
        }
        if (strcmp(argv[index], "--cmd") == 0)
        {
            command = argv[index + 1];
        }
        else if (strcmp(argv[index], "--host") == 0)
        {
            host = argv[index + 1];
        }
        else if (strcmp(argv[index], "--port") == 0)
        {
            if (parse_port(argv[index + 1], &port) < 0)
            {
                return -1;
            }
        }
        else if ((strcmp(argv[index], "--file") == 0 || strcmp(argv[index], "--name") == 0))
        {
            file = argv[index + 1];
        }
        else if (strcmp(argv[index], "--output") == 0)
        {
            output = argv[index + 1];
        }
        else
        {
            errno = EINVAL;
            return -1;
        }
    }
    if (command != NULL && (strcmp(command, "upload") == 0 || strcmp(command, "download") == 0))
    {
        if (host == NULL) host = "127.0.0.1";
        if (port == 0U) port = strcmp(command, "upload") == 0 ? DEFAULT_PEER_PORT : DEFAULT_SUPERPEER_PORT;
    }
    if (command == NULL || host == NULL || port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(command, "upload") == 0)
    {
        if (file == NULL)
        {
            errno = EINVAL;
            return -1;
        }
        return local_control_command(local_peer_port, 1, file, NULL, host, port);
    }
    if (strcmp(command, "download") == 0)
    {
        if (file == NULL)
        {
            errno = EINVAL;
            return -1;
        }
        return local_control_command(local_peer_port, 0, file, output, host, port);
    }
    if (file != NULL || output != NULL)
    {
        errno = EINVAL;
        return -1;
    }
    return legacy_command(command, host, port);
}

static void usage(const char *program)
{
    fprintf(stderr, "Uso:\n  %s serve <porta-peer> <host-superpeer> <porta-superpeer>\n  %s upload <arquivo.pdf> [<host-peer> <porta-peer>]\n  %s download <nome-ou-objectid> [<destino>] [<host-superpeer> <porta-superpeer>]\n  %s benchmark <arquivo.pdf>\n  %s --cmd <ping|join|leave|upload|download> --host <ip> --port <porta> [--file <arquivo>] [--output <destino>]\n", program, program, program, program, program);
}

int main(int argc, char **argv)
{
    int result = -1;

    if (app_config_load(&argc, argv, 0) < 0) { perror("config"); return EXIT_FAILURE; }
    (void)signal(SIGPIPE, SIG_IGN);
    /* Remove a opção transversal antes de interpretar a CLI legada. */
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--local-peer-port") == 0)
        {
            if (i + 1 >= argc || parse_port(argv[i + 1], &local_peer_port) < 0) goto failure;
            for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            argv[argc] = NULL;
            --i;
        }
    }
    if (argc >= 2 && strcmp(argv[1], "--cmd") == 0)
    {
        result = option_command(argc, argv);
    }
    else if (argc == 2 && strcmp(argv[1], "serve") == 0)
    {
        return peer_service_run(app_config.port, app_config.superpeer, app_config.superpeer_port);
    }
    else if (argc == 5 && strcmp(argv[1], "serve") == 0)
    {
        uint16_t local_port;
        uint16_t superpeer_port;

        if (parse_port(argv[2], &local_port) == 0 && parse_port(argv[4], &superpeer_port) == 0)
        {
            return peer_service_run(local_port, argv[3], superpeer_port);
        }
    }
    else if ((argc == 3 || argc == 5) && strcmp(argv[1], "upload") == 0)
    {
        const char *host = argc == 5 ? argv[3] : DEFAULT_PEER_HOST;
        uint16_t port = DEFAULT_PEER_PORT;

        if ((argc == 3 || parse_port(argv[4], &port) == 0))
        {
            result = local_control_command(local_peer_port, 1, argv[2], NULL, host, port);
        }
    }
    else if (argc == 3 && strcmp(argv[1], "benchmark") == 0)
    {
        result = file_client_benchmark_lz4(argv[2]);
    }
    else if ((argc == 3 || argc == 4 || argc == 5 || argc == 6) && strcmp(argv[1], "download") == 0)
    {
        const char *destination = NULL;
        const char *host = DEFAULT_SUPERPEER_HOST;
        uint16_t port = DEFAULT_SUPERPEER_PORT;

        if (argc == 4)
        {
            destination = argv[3];
        }
        else if (argc == 5)
        {
            host = argv[3];
            if (parse_port(argv[4], &port) < 0)
            {
                goto failure;
            }
        }
        else if (argc == 6)
        {
            destination = argv[3];
            host = argv[4];
            if (parse_port(argv[5], &port) < 0)
            {
                goto failure;
            }
        }
        result = local_control_command(local_peer_port, 0, argv[2], destination, host, port);
    }
    if (result == 0)
    {
        return EXIT_SUCCESS;
    }

failure:
    if (errno != 0)
    {
        perror("peer");
    }
    usage(argv[0]);
    return EXIT_FAILURE;
}
