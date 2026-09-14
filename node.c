#define _POSIX_C_SOURCE 200809L

#include "node.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
    uint8_t data[64];
    uint32_t data_length;
    uint64_t bit_length;
    uint32_t state[8];
} Sha256Context;

static const uint32_t sha256_constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t sha256_rotate_right(uint32_t value, uint32_t amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t sha256_choose(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static uint32_t sha256_majority(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t sha256_sigma0(uint32_t value)
{
    return sha256_rotate_right(value, 2U) ^
           sha256_rotate_right(value, 13U) ^
           sha256_rotate_right(value, 22U);
}

static uint32_t sha256_sigma1(uint32_t value)
{
    return sha256_rotate_right(value, 6U) ^
           sha256_rotate_right(value, 11U) ^
           sha256_rotate_right(value, 25U);
}

static uint32_t sha256_gamma0(uint32_t value)
{
    return sha256_rotate_right(value, 7U) ^
           sha256_rotate_right(value, 18U) ^
           (value >> 3U);
}

static uint32_t sha256_gamma1(uint32_t value)
{
    return sha256_rotate_right(value, 17U) ^
           sha256_rotate_right(value, 19U) ^
           (value >> 10U);
}

static void sha256_transform(Sha256Context *context)
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t i;

    for (i = 0U; i < 16U; ++i)
    {
        uint32_t offset = i * 4U;

        words[i] = ((uint32_t)context->data[offset] << 24U) |
                   ((uint32_t)context->data[offset + 1U] << 16U) |
                   ((uint32_t)context->data[offset + 2U] << 8U) |
                   (uint32_t)context->data[offset + 3U];
    }

    for (i = 16U; i < 64U; ++i)
    {
        words[i] = sha256_gamma1(words[i - 2U]) + words[i - 7U] +
                   sha256_gamma0(words[i - 15U]) + words[i - 16U];
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (i = 0U; i < 64U; ++i)
    {
        uint32_t temporary1 = h + sha256_sigma1(e) +
                              sha256_choose(e, f, g) +
                              sha256_constants[i] + words[i];
        uint32_t temporary2 = sha256_sigma0(a) + sha256_majority(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(Sha256Context *context)
{
    context->data_length = 0U;
    context->bit_length = 0U;
    context->state[0] = 0x6a09e667U;
    context->state[1] = 0xbb67ae85U;
    context->state[2] = 0x3c6ef372U;
    context->state[3] = 0xa54ff53aU;
    context->state[4] = 0x510e527fU;
    context->state[5] = 0x9b05688cU;
    context->state[6] = 0x1f83d9abU;
    context->state[7] = 0x5be0cd19U;
}

static void sha256_update(Sha256Context *context,
                          const uint8_t *data,
                          size_t data_length)
{
    size_t i;

    for (i = 0U; i < data_length; ++i)
    {
        context->data[context->data_length] = data[i];
        ++context->data_length;

        if (context->data_length == sizeof(context->data))
        {
            sha256_transform(context);
            context->bit_length += 512U;
            context->data_length = 0U;
        }
    }
}

static void sha256_final(Sha256Context *context, uint8_t digest[NODE_ID_SIZE])
{
    uint32_t i = context->data_length;

    context->data[i] = 0x80U;
    ++i;

    if (i > 56U)
    {
        while (i < 64U)
        {
            context->data[i] = 0U;
            ++i;
        }
        sha256_transform(context);
        i = 0U;
    }

    while (i < 56U)
    {
        context->data[i] = 0U;
        ++i;
    }

    context->bit_length += (uint64_t)context->data_length * 8U;
    context->data[56] = (uint8_t)(context->bit_length >> 56U);
    context->data[57] = (uint8_t)(context->bit_length >> 48U);
    context->data[58] = (uint8_t)(context->bit_length >> 40U);
    context->data[59] = (uint8_t)(context->bit_length >> 32U);
    context->data[60] = (uint8_t)(context->bit_length >> 24U);
    context->data[61] = (uint8_t)(context->bit_length >> 16U);
    context->data[62] = (uint8_t)(context->bit_length >> 8U);
    context->data[63] = (uint8_t)context->bit_length;
    sha256_transform(context);

    for (i = 0U; i < 8U; ++i)
    {
        digest[i * 4U] = (uint8_t)(context->state[i] >> 24U);
        digest[i * 4U + 1U] = (uint8_t)(context->state[i] >> 16U);
        digest[i * 4U + 2U] = (uint8_t)(context->state[i] >> 8U);
        digest[i * 4U + 3U] = (uint8_t)context->state[i];
    }
}

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
        ssize_t bytes_read = read(random_fd,
                                  uuid + total_read,
                                  NODE_UUID_SIZE - total_read);

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

int node_config_init_with_uuid(NodeConfig *config,
                               const char *ip,
                               uint16_t port,
                               const uint8_t uuid[NODE_UUID_SIZE])
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

int node_compute_id(const NodeConfig *config, NodeID *id)
{
    struct in_addr ipv4;
    struct in6_addr ipv6;
    const uint8_t *address_bytes;
    size_t address_size;
    uint16_t network_port;
    Sha256Context context;

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
    sha256_init(&context);
    sha256_update(&context, address_bytes, address_size);
    sha256_update(&context, (const uint8_t *)&network_port, sizeof(network_port));
    sha256_update(&context, config->uuid, NODE_UUID_SIZE);
    sha256_final(&context, id->bytes);
    return 0;
}

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

int node_validate(const Node *node)
{
    NodeID expected_id;

    if (node == NULL || node->process_id <= 0 ||
        node_compute_id(&node->config, &expected_id) == -1 ||
        !node_id_equal(&node->id, &expected_id))
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

int node_id_equal(const NodeID *left, const NodeID *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, NODE_ID_SIZE) == 0;
}

pid_t node_get_process_id(const Node *node)
{
    return node == NULL ? (pid_t)-1 : node->process_id;
}
