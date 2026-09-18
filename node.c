#define _POSIX_C_SOURCE 200809L

#include "node.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/*
 * A execução atual fornece libcrypto.so.3, mas não os headers de
 * desenvolvimento do OpenSSL. Esta é a assinatura pública de SHA256(); a
 * implementação continua sendo fornecida pela biblioteca externa.
 */
extern unsigned char *SHA256(const unsigned char *data, size_t size, unsigned char *digest);

/* Valida IPv4/IPv6 e normaliza o texto do endereço. */
static int normalize_ip(const char *ip, char normalized[NODE_ADDRESS_SIZE])
{
    struct in_addr ipv4;
    struct in6_addr ipv6;

    if (ip == NULL || normalized == NULL || ip[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    memset(normalized, 0, NODE_ADDRESS_SIZE);

    if (inet_pton(AF_INET, ip, &ipv4) == 1)
    {
        if (inet_ntop(AF_INET, &ipv4, normalized, NODE_ADDRESS_SIZE) == NULL)
        {
            return -1;
        }
        return 0;
    }

    if (inet_pton(AF_INET6, ip, &ipv6) == 1)
    {
        if (inet_ntop(AF_INET6, &ipv6, normalized, NODE_ADDRESS_SIZE) == NULL)
        {
            return -1;
        }
        return 0;
    }

    errno = EINVAL;
    return -1;
}

/* Lê 16 bytes de /dev/urandom, trata leitura parcial/EINTR e ajusta UUID v4. */
int node_generate_uuid(uint8_t uuid[NODE_UUID_SIZE])
{
    int random_fd;
    size_t total_read = 0U;

    if (uuid == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    /* The assignment targets Linux, where /dev/urandom is available. */
    random_fd = open("/dev/urandom", O_RDONLY);
    if (random_fd == -1)
    {
        return -1;
    }

    while (total_read < NODE_UUID_SIZE)
    {
        ssize_t bytes_read = read(random_fd, uuid + total_read, NODE_UUID_SIZE - total_read);

        if (bytes_read > 0)
        {
            total_read += (size_t)bytes_read;
        }
        else if (bytes_read == -1 && errno == EINTR)
        {
            continue;
        }
        else
        {
            int saved_errno = bytes_read == 0 ? EIO : errno;

            close(random_fd);
            errno = saved_errno;
            return -1;
        }
    }

    if (close(random_fd) == -1)
    {
        return -1;
    }

    /* RFC 4122 version 4 and variant bits. */
    uuid[6] = (uint8_t)((uuid[6] & 0x0fU) | 0x40U);
    uuid[8] = (uint8_t)((uuid[8] & 0x3fU) | 0x80U);
    return 0;
}

/* Exige porta não nula e IP válido com terminador dentro do buffer. */
int node_config_validate(const NodeConfig *config)
{
    char normalized[NODE_ADDRESS_SIZE];

    if (config == NULL || config->port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (memchr(config->ip, '\0', sizeof(config->ip)) == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    return normalize_ip(config->ip, normalized);
}

/* Preserva o UUID fornecido para reconstruir uma identidade conhecida. */
int node_config_init_with_uuid(NodeConfig *config, const char *ip, uint16_t port, const uint8_t uuid[NODE_UUID_SIZE])
{
    char normalized[NODE_ADDRESS_SIZE];

    if (config == NULL || uuid == NULL || port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (normalize_ip(ip, normalized) == -1)
    {
        return -1;
    }

    memset(config, 0, sizeof(*config));
    memcpy(config->ip, normalized, sizeof(config->ip));
    config->port = port;
    memcpy(config->uuid, uuid, NODE_UUID_SIZE);
    return 0;
}

/* Gera UUID novo e normaliza a configuração; não persiste identidade em disco. */
int node_config_init(NodeConfig *config, const char *ip, uint16_t port)
{
    uint8_t uuid[NODE_UUID_SIZE];

    if (config == NULL || ip == NULL || port == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_generate_uuid(uuid) == -1)
    {
        return -1;
    }

    return node_config_init_with_uuid(config, ip, port, uuid);
}

/* Calcula SHA-256(IP binário || porta em ordem de rede || UUID); exclui PID e papel. */
int node_compute_id(const NodeConfig *config, NodeID *id)
{
    struct in_addr ipv4;
    struct in6_addr ipv6;
    const uint8_t *address_bytes;
    size_t address_size;
    uint16_t network_port;
    uint8_t input[sizeof(struct in6_addr) + sizeof(uint16_t) + NODE_UUID_SIZE];
    size_t input_size = 0U;

    if (id == NULL || config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_config_validate(config) == -1)
    {
        return -1;
    }

    if (inet_pton(AF_INET, config->ip, &ipv4) == 1)
    {
        address_bytes = (const uint8_t *)&ipv4;
        address_size = sizeof(ipv4);
    }
    else if (inet_pton(AF_INET6, config->ip, &ipv6) == 1)
    {
        address_bytes = (const uint8_t *)&ipv6;
        address_size = sizeof(ipv6);
    }
    else
    {
        errno = EINVAL;
        return -1;
    }

    network_port = htons(config->port);
    memcpy(input + input_size, address_bytes, address_size);
    input_size += address_size;
    memcpy(input + input_size, &network_port, sizeof(network_port));
    input_size += sizeof(network_port);
    memcpy(input + input_size, config->uuid, NODE_UUID_SIZE);
    input_size += NODE_UUID_SIZE;

    if (SHA256(input, input_size, id->bytes) == NULL)
    {
        errno = EIO;
        return -1;
    }

    return 0;
}

/* Calcula ID, copia configuração e registra getpid(), inicialmente com papel peer. */
int node_init(Node *node, const NodeConfig *config)
{
    if (node == NULL || config == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (node_compute_id(config, &node->id) == -1)
    {
        return -1;
    }

    node->config = *config;
    node->process_id = getpid();
    node->role = NODE_ROLE_PEER;
    return 0;
}

/* Recalcula ID e valida PID/papel; verifica consistência, não autenticação. */
int node_validate(const Node *node)
{
    NodeID expected_id;

    if (node == NULL || node->process_id <= 0 || node_compute_id(&node->config, &expected_id) == -1 || !node_id_equal(&node->id, &expected_id))
    {
        errno = EINVAL;
        return -1;
    }

    if (node->role != NODE_ROLE_PEER && node->role != NODE_ROLE_SUPERPEER)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/* Converte 32 bytes em 64 dígitos hexadecimais mais terminador zero. */
int node_id_to_hex(const NodeID *id, char *output, size_t output_size)
{
    static const char hexadecimal[] = "0123456789abcdef";
    size_t i;

    if (id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (output_size < NODE_ID_HEX_SIZE)
    {
        errno = ENOSPC;
        return -1;
    }

    for (i = 0U; i < NODE_ID_SIZE; ++i)
    {
        output[i * 2U] = hexadecimal[id->bytes[i] >> 4U];
        output[i * 2U + 1U] = hexadecimal[id->bytes[i] & 0x0fU];
    }
    output[NODE_ID_HEX_SIZE - 1U] = '\0';
    return 0;
}

/* Decodifica um dígito hexadecimal, incluindo letras maiúsculas. */
static int hexadecimal_value(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

/* Exige exatamente 64 dígitos válidos e reconstrói o ID binário. */
int node_id_from_hex(NodeID *id, const char *hex)
{
    size_t i;

    if (id == NULL || hex == NULL || strlen(hex) != NODE_ID_HEX_SIZE - 1U)
    {
        errno = EINVAL;
        return -1;
    }

    for (i = 0U; i < NODE_ID_SIZE; ++i)
    {
        int high = hexadecimal_value(hex[i * 2U]);
        int low = hexadecimal_value(hex[i * 2U + 1U]);

        if (high < 0 || low < 0)
        {
            errno = EINVAL;
            return -1;
        }
        id->bytes[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

/* Compara IDs lexicograficamente; retorna -1, 0 ou 1. Não implementa eleição. */
int node_id_compare(const NodeID *left, const NodeID *right)
{
    int comparison;

    if (left == NULL && right == NULL)
    {
        return 0;
    }
    if (left == NULL)
    {
        return -1;
    }
    if (right == NULL)
    {
        return 1;
    }

    comparison = memcmp(left->bytes, right->bytes, NODE_ID_SIZE);
    if (comparison < 0)
    {
        return -1;
    }
    if (comparison > 0)
    {
        return 1;
    }
    return 0;
}

/* Compara os 32 bytes; ponteiros nulos não são considerados IDs iguais. */
int node_id_equal(const NodeID *left, const NodeID *right)
{
    return left != NULL && right != NULL && memcmp(left->bytes, right->bytes, NODE_ID_SIZE) == 0;
}

/* Consulta o PID armazenado; retorna -1 para nó nulo. */
pid_t node_get_process_id(const Node *node)
{
    return node == NULL ? (pid_t)-1 : node->process_id;
}
