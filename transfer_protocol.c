#include "wire.h"
#include "transfer_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DOCUMENT_FIXED_SIZE (OBJECT_ID_SIZE + 8U + 8U + 1U + 2U)
#define CHUNK_FIXED_SIZE (1U + OBJECT_ID_SIZE + 8U + 8U + 4U + 4U + OBJECT_ID_SIZE)
#define ENDPOINT_FIXED_SIZE (NODE_ID_SIZE + 1U + 2U)

static atomic_uint_least32_t transaction_sequence = 0;
static atomic_uint_fast64_t transaction_clock = 0;

#define DESCRIPTOR_SIZE 56U
static void encode_descriptor(uint8_t *output, const MetadataChunk *chunk);
static void decode_descriptor(const uint8_t *input, MetadataChunk *chunk);

static size_t bounded_string_length(const char *text, size_t capacity)
{
    size_t length = 0U;

    while (length < capacity && text[length] != '\0')
    {
        ++length;
    }
    return length;
}







static int object_id_from_hex(ObjectID *id, const char *text)
{
    size_t index;

    if (id == NULL || text == NULL || strlen(text) != OBJECT_ID_SIZE * 2U)
    {
        return -1;
    }
    for (index = 0U; index < OBJECT_ID_SIZE; ++index)
    {
        int high;
        int low;
        char a = text[index * 2U];
        char b = text[index * 2U + 1U];

        high = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1;
        low = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
        if (high < 0 || low < 0)
        {
            return -1;
        }
        id->bytes[index] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

static size_t document_wire_size(const TransferDocument *document)
{
    return 1U + DOCUMENT_FIXED_SIZE + bounded_string_length(document->name, METADATA_NAME_SIZE);
}

static int encode_document_at(const TransferDocument *document, uint8_t operation, uint8_t *output, size_t capacity, size_t *used)
{
    size_t name_size;
    size_t offset = 0U;

    if (document == NULL || output == NULL || used == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    name_size = bounded_string_length(document->name, METADATA_NAME_SIZE);
    if (name_size == 0U || name_size >= METADATA_NAME_SIZE || document->compression != COMPRESSION_LZ4 || document->chunk_count != document->file_size / METADATA_CHUNK_SIZE + (document->file_size % METADATA_CHUNK_SIZE != 0U) || capacity < 1U + DOCUMENT_FIXED_SIZE + name_size)
    {
        errno = EINVAL;
        return -1;
    }
    output[offset++] = operation;
    memcpy(output + offset, document->id.bytes, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    wire_put_u64(output + offset, document->file_size);
    offset += 8U;
    wire_put_u64(output + offset, document->chunk_count);
    offset += 8U;
    output[offset++] = document->compression;
    wire_put_u16(output + offset, (uint16_t)name_size);
    offset += 2U;
    memcpy(output + offset, document->name, name_size);
    offset += name_size;
    *used = offset;
    return 0;
}

static int decode_document_at(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document, size_t *used)
{
    size_t offset = 0U;
    uint16_t name_size;

    if (payload == NULL || document == NULL || used == NULL || payload_size < 1U + DOCUMENT_FIXED_SIZE || payload[offset++] != expected_operation)
    {
        errno = EBADMSG;
        return -1;
    }
    memset(document, 0, sizeof(*document));
    memcpy(document->id.bytes, payload + offset, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    document->file_size = wire_get_u64(payload + offset);
    offset += 8U;
    document->chunk_count = wire_get_u64(payload + offset);
    offset += 8U;
    document->compression = payload[offset++];
    name_size = wire_get_u16(payload + offset);
    offset += 2U;
    if (name_size == 0U || name_size >= METADATA_NAME_SIZE || offset + name_size > payload_size || memchr(payload + offset, '\0', name_size) != NULL || memchr(payload + offset, '/', name_size) != NULL || document->compression != COMPRESSION_LZ4 || document->chunk_count != document->file_size / METADATA_CHUNK_SIZE + (document->file_size % METADATA_CHUNK_SIZE != 0U))
    {
        errno = EBADMSG;
        return -1;
    }
    memcpy(document->name, payload + offset, name_size);
    document->name[name_size] = '\0';
    offset += name_size;
    *used = offset;
    return 0;
}

void transfer_fill_transaction_id(uint8_t output[TRANSACTION_ID_SIZE], const uint8_t source_node[NODE_ID_SIZE])
{
    struct timespec now;
    (void)timespec_get(&now, TIME_UTC);
    uint64_t timestamp = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    uint_fast64_t previous = atomic_load_explicit(&transaction_clock, memory_order_relaxed);
    for (;;)
    {
        if (timestamp <= previous) timestamp = previous + 1U;
        if (atomic_compare_exchange_weak_explicit(&transaction_clock, &previous, timestamp, memory_order_relaxed, memory_order_relaxed)) break;
    }
    uint32_t sequence = atomic_fetch_add_explicit(&transaction_sequence, 1U, memory_order_relaxed) + 1U;

    wire_put_u64(output, timestamp);
    if (source_node == NULL)
    {
        memset(output + 8U, 0, 4U);
    }
    else
    {
        memcpy(output + 8U, source_node, 4U);
    }
    wire_put_u32(output + 12U, sequence);
}

int transfer_encode_document(const TransferDocument *document, uint8_t **output, uint32_t *output_size, uint8_t operation)
{
    size_t size;
    size_t used;
    uint8_t *buffer;

    if (document == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    size = document_wire_size(document);
    if (size > MAX_PAYLOAD_SIZE || size > UINT32_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    buffer = malloc(size);
    if (buffer == NULL)
    {
        return -1;
    }
    if (encode_document_at(document, operation, buffer, size, &used) < 0)
    {
        free(buffer);
        return -1;
    }
    *output = buffer;
    *output_size = (uint32_t)used;
    return 0;
}

int transfer_decode_document(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document)
{
    size_t used;

    if (decode_document_at(payload, payload_size, expected_operation, document, &used) < 0 || used != payload_size)
    {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

int transfer_encode_chunk(const TransferChunk *chunk, uint8_t operation, uint8_t **output, uint32_t *output_size)
{
    size_t size;
    size_t offset = 0U;
    uint8_t *buffer;

    if (chunk == NULL || output == NULL || output_size == NULL || chunk->data == NULL || chunk->raw_size == 0U || chunk->compressed_size == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    size = CHUNK_FIXED_SIZE + chunk->compressed_size;
    if (size > MAX_PAYLOAD_SIZE || size > UINT32_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    buffer = malloc(size);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[offset++] = operation;
    memcpy(buffer + offset, chunk->id.bytes, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    wire_put_u64(buffer + offset, chunk->index);
    offset += 8U;
    wire_put_u64(buffer + offset, chunk->offset);
    offset += 8U;
    wire_put_u32(buffer + offset, chunk->raw_size);
    offset += 4U;
    wire_put_u32(buffer + offset, chunk->compressed_size);
    offset += 4U;
    memcpy(buffer + offset, chunk->hash, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    memcpy(buffer + offset, chunk->data, chunk->compressed_size);
    *output = buffer;
    *output_size = (uint32_t)size;
    return 0;
}

int transfer_decode_chunk(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferChunk *chunk)
{
    size_t offset = 0U;

    if (payload == NULL || chunk == NULL || payload_size < CHUNK_FIXED_SIZE || payload[offset++] != expected_operation)
    {
        errno = EBADMSG;
        return -1;
    }
    memset(chunk, 0, sizeof(*chunk));
    memcpy(chunk->id.bytes, payload + offset, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    chunk->index = wire_get_u64(payload + offset);
    offset += 8U;
    chunk->offset = wire_get_u64(payload + offset);
    offset += 8U;
    chunk->raw_size = wire_get_u32(payload + offset);
    offset += 4U;
    chunk->compressed_size = wire_get_u32(payload + offset);
    offset += 4U;
    memcpy(chunk->hash, payload + offset, OBJECT_ID_SIZE);
    offset += OBJECT_ID_SIZE;
    if (chunk->raw_size == 0U || chunk->raw_size > METADATA_CHUNK_SIZE || chunk->compressed_size == 0U || offset + chunk->compressed_size != payload_size)
    {
        errno = EBADMSG;
        return -1;
    }
    chunk->data = payload + offset;
    return 0;
}

int transfer_encode_object_operation(uint8_t operation, const ObjectID *id, uint8_t **output, uint32_t *output_size)
{
    uint8_t *buffer;

    if (id == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    buffer = malloc(1U + OBJECT_ID_SIZE);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[0] = operation;
    memcpy(buffer + 1U, id->bytes, OBJECT_ID_SIZE);
    *output = buffer;
    *output_size = 1U + OBJECT_ID_SIZE;
    return 0;
}

int transfer_decode_object_operation(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, ObjectID *id)
{
    if (payload == NULL || id == NULL || payload_size != 1U + OBJECT_ID_SIZE || payload[0] != expected_operation)
    {
        errno = EBADMSG;
        return -1;
    }
    memcpy(id->bytes, payload + 1U, OBJECT_ID_SIZE);
    return 0;
}

int transfer_encode_lookup_request(const char *selector, uint8_t **output, uint32_t *output_size)
{
    ObjectID id;
    size_t length;
    uint8_t type;
    uint8_t *buffer;

    if (selector == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    length = strlen(selector);
    type = object_id_from_hex(&id, selector) == 0 ? TRANSFER_SELECTOR_OBJECT_ID : TRANSFER_SELECTOR_NAME;
    if (type == TRANSFER_SELECTOR_OBJECT_ID)
    {
        length = OBJECT_ID_SIZE;
    }
    else if (length == 0U || length >= METADATA_NAME_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    buffer = malloc(3U + length);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[0] = type;
    wire_put_u16(buffer + 1U, (uint16_t)length);
    if (type == TRANSFER_SELECTOR_OBJECT_ID)
    {
        memcpy(buffer + 3U, id.bytes, OBJECT_ID_SIZE);
    }
    else
    {
        memcpy(buffer + 3U, selector, length);
    }
    *output = buffer;
    *output_size = (uint32_t)(3U + length);
    return 0;
}

int transfer_decode_lookup_request(const uint8_t *payload, size_t payload_size, TransferSelectorType *type, ObjectID *id, char name[METADATA_NAME_SIZE])
{
    uint16_t length;

    if (payload == NULL || type == NULL || id == NULL || name == NULL || payload_size < 3U)
    {
        errno = EBADMSG;
        return -1;
    }
    length = wire_get_u16(payload + 1U);
    if ((payload[0] != TRANSFER_SELECTOR_OBJECT_ID && payload[0] != TRANSFER_SELECTOR_NAME) || 3U + length != payload_size)
    {
        errno = EBADMSG;
        return -1;
    }
    memset(id, 0, sizeof(*id));
    memset(name, 0, METADATA_NAME_SIZE);
    if (payload[0] == TRANSFER_SELECTOR_OBJECT_ID)
    {
        if (length != OBJECT_ID_SIZE)
        {
            errno = EBADMSG;
            return -1;
        }
        memcpy(id->bytes, payload + 3U, OBJECT_ID_SIZE);
    }
    else
    {
        if (length == 0U || length >= METADATA_NAME_SIZE || memchr(payload + 3U, '\0', length) != NULL || memchr(payload + 3U, '/', length) != NULL)
        {
            errno = EBADMSG;
            return -1;
        }
        memcpy(name, payload + 3U, length);
    }
    *type = (TransferSelectorType)payload[0];
    return 0;
}

void transfer_lookup_result_free(TransferLookupResult *result)
{
    uint64_t index;

    if (result == NULL)
    {
        return;
    }
    if (result->chunks != NULL)
    {
        for (index = 0U; index < result->document.chunk_count; ++index)
        {
            free(result->chunks[index].peers);
        }
    }
    free(result->chunks);
    memset(result, 0, sizeof(*result));
}

int transfer_encode_lookup_result(const TransferLookupResult *result, uint8_t **output, uint32_t *output_size)
{
    size_t size;
    size_t used;
    size_t offset;
    uint64_t chunk_index;
    uint8_t *buffer;

    if (result == NULL || output == NULL || output_size == NULL || (result->document.chunk_count != 0U && result->chunks == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    size = document_wire_size(&result->document);
    for (chunk_index = 0U; chunk_index < result->document.chunk_count; ++chunk_index)
    {
        size_t peer_index;

        if (result->chunks[chunk_index].peer_count > UINT16_MAX)
        {
            errno = EOVERFLOW;
            return -1;
        }
        if (size > SIZE_MAX - 2U - DESCRIPTOR_SIZE)
        {
            errno = EOVERFLOW;
            return -1;
        }
        size += 2U + DESCRIPTOR_SIZE;
        for (peer_index = 0U; peer_index < result->chunks[chunk_index].peer_count; ++peer_index)
        {
            size_t ip_size = bounded_string_length(result->chunks[chunk_index].peers[peer_index].ip, NODE_ADDRESS_SIZE);

            if (ip_size == 0U || ip_size >= NODE_ADDRESS_SIZE || size > SIZE_MAX - ENDPOINT_FIXED_SIZE - ip_size)
            {
                errno = EINVAL;
                return -1;
            }
            size += ENDPOINT_FIXED_SIZE + ip_size;
        }
    }
    if (size > MAX_PAYLOAD_SIZE || size > UINT32_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    buffer = malloc(size);
    if (buffer == NULL)
    {
        return -1;
    }
    if (encode_document_at(&result->document, TRANSFER_DOWNLOAD_METADATA, buffer, size, &used) < 0)
    {
        free(buffer);
        return -1;
    }
    offset = used;
    for (chunk_index = 0U; chunk_index < result->document.chunk_count; ++chunk_index)
    {
        size_t peer_index;

        encode_descriptor(buffer + offset, &result->chunks[chunk_index].descriptor);
        offset += DESCRIPTOR_SIZE;
        wire_put_u16(buffer + offset, (uint16_t)result->chunks[chunk_index].peer_count);
        offset += 2U;
        for (peer_index = 0U; peer_index < result->chunks[chunk_index].peer_count; ++peer_index)
        {
            const TransferEndpoint *endpoint = &result->chunks[chunk_index].peers[peer_index];
            size_t ip_size = strlen(endpoint->ip);

            memcpy(buffer + offset, endpoint->node_id.bytes, NODE_ID_SIZE);
            offset += NODE_ID_SIZE;
            buffer[offset++] = (uint8_t)ip_size;
            memcpy(buffer + offset, endpoint->ip, ip_size);
            offset += ip_size;
            wire_put_u16(buffer + offset, endpoint->port);
            offset += 2U;
        }
    }
    *output = buffer;
    *output_size = (uint32_t)offset;
    return 0;
}

int transfer_decode_lookup_result(const uint8_t *payload, size_t payload_size, TransferLookupResult *result)
{
    size_t offset;
    uint64_t chunk_index;

    if (payload == NULL || result == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (decode_document_at(payload, payload_size, TRANSFER_DOWNLOAD_METADATA, &result->document, &offset) < 0 || result->document.chunk_count > SIZE_MAX / sizeof(*result->chunks))
    {
        return -1;
    }
    if (result->document.chunk_count > (payload_size - offset) / (DESCRIPTOR_SIZE + 2U))
    {
        errno = EBADMSG;
        return -1;
    }
    if (result->document.chunk_count != 0U)
    {
        result->chunks = calloc((size_t)result->document.chunk_count, sizeof(*result->chunks));
        if (result->chunks == NULL)
        {
            return -1;
        }
    }
    for (chunk_index = 0U; chunk_index < result->document.chunk_count; ++chunk_index)
    {
        uint16_t peer_count;
        size_t peer_index;

        if (offset + DESCRIPTOR_SIZE + 2U > payload_size)
        {
            goto invalid;
        }
        decode_descriptor(payload + offset, &result->chunks[chunk_index].descriptor);
        if (result->chunks[chunk_index].descriptor.index != chunk_index) goto invalid;
        offset += DESCRIPTOR_SIZE;
        peer_count = wire_get_u16(payload + offset);
        offset += 2U;
        result->chunks[chunk_index].peer_count = peer_count;
        if (peer_count != 0U)
        {
            result->chunks[chunk_index].peers = calloc(peer_count, sizeof(*result->chunks[chunk_index].peers));
            if (result->chunks[chunk_index].peers == NULL)
            {
                transfer_lookup_result_free(result);
                return -1;
            }
        }
        for (peer_index = 0U; peer_index < peer_count; ++peer_index)
        {
            TransferEndpoint *endpoint = &result->chunks[chunk_index].peers[peer_index];
            uint8_t ip_size;

            if (offset + ENDPOINT_FIXED_SIZE > payload_size)
            {
                goto invalid;
            }
            memcpy(endpoint->node_id.bytes, payload + offset, NODE_ID_SIZE);
            offset += NODE_ID_SIZE;
            ip_size = payload[offset++];
            if (ip_size == 0U || ip_size >= NODE_ADDRESS_SIZE || offset + ip_size + 2U > payload_size)
            {
                goto invalid;
            }
            memcpy(endpoint->ip, payload + offset, ip_size);
            endpoint->ip[ip_size] = '\0';
            offset += ip_size;
            endpoint->port = wire_get_u16(payload + offset);
            offset += 2U;
            if (endpoint->port == 0U)
            {
                goto invalid;
            }
        }
    }
    if (offset != payload_size)
    {
        goto invalid;
    }
    return 0;

invalid:
    transfer_lookup_result_free(result);
    errno = EBADMSG;
    return -1;
}

int transfer_encode_chunk_request(const ObjectID *id, uint64_t index, uint8_t **output, uint32_t *output_size)
{
    uint8_t *buffer;

    if (id == NULL || output == NULL || output_size == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    buffer = malloc(1U + OBJECT_ID_SIZE + 8U);
    if (buffer == NULL)
    {
        return -1;
    }
    buffer[0] = TRANSFER_DOWNLOAD_CHUNK;
    memcpy(buffer + 1U, id->bytes, OBJECT_ID_SIZE);
    wire_put_u64(buffer + 1U + OBJECT_ID_SIZE, index);
    *output = buffer;
    *output_size = 1U + OBJECT_ID_SIZE + 8U;
    return 0;
}

int transfer_decode_chunk_request(const uint8_t *payload, size_t payload_size, ObjectID *id, uint64_t *index)
{
    if (payload == NULL || id == NULL || index == NULL || payload_size != 1U + OBJECT_ID_SIZE + 8U || payload[0] != TRANSFER_DOWNLOAD_CHUNK)
    {
        errno = EBADMSG;
        return -1;
    }
    memcpy(id->bytes, payload + 1U, OBJECT_ID_SIZE);
    *index = wire_get_u64(payload + 1U + OBJECT_ID_SIZE);
    return 0;
}

static void encode_descriptor(uint8_t *output, const MetadataChunk *chunk)
{
    wire_put_u64(output, chunk->index);
    wire_put_u64(output + 8U, chunk->offset);
    wire_put_u32(output + 16U, chunk->raw_size);
    wire_put_u32(output + 20U, chunk->compressed_size);
    memcpy(output + 24U, chunk->hash, OBJECT_ID_SIZE);
}

static void decode_descriptor(const uint8_t *input, MetadataChunk *chunk)
{
    chunk->index = wire_get_u64(input);
    chunk->offset = wire_get_u64(input + 8U);
    chunk->raw_size = wire_get_u32(input + 16U);
    chunk->compressed_size = wire_get_u32(input + 20U);
    memcpy(chunk->hash, input + 24U, OBJECT_ID_SIZE);
}

int transfer_encode_announcement(const TransferDocument *document, const MetadataChunk *chunks, uint8_t **output, uint32_t *size)
{
    if (document == NULL || chunks == NULL || output == NULL || size == NULL) { errno = EINVAL; return -1; }
    *output = NULL;
    size_t base = document_wire_size(document), used;
    if (base > MAX_PAYLOAD_SIZE || document->chunk_count > (MAX_PAYLOAD_SIZE - base) / DESCRIPTOR_SIZE) { errno = EMSGSIZE; return -1; }
    size_t total = base + (size_t)document->chunk_count * DESCRIPTOR_SIZE;
    uint8_t *buffer = malloc(total);
    if (buffer == NULL) return -1;
    if (encode_document_at(document, TRANSFER_STORE_ANNOUNCE, buffer, total, &used) < 0) { free(buffer); return -1; }
    for (uint64_t i = 0U; i < document->chunk_count; ++i) encode_descriptor(buffer + used + (size_t)i * DESCRIPTOR_SIZE, &chunks[i]);
    *output = buffer;
    *size = (uint32_t)total;
    return 0;
}

int transfer_decode_announcement(const uint8_t *payload, size_t size, TransferDocument *document, MetadataChunk **chunks)
{
    size_t used;
    if (chunks == NULL || document == NULL) { errno = EINVAL; return -1; }
    *chunks = NULL;
    if (decode_document_at(payload, size, TRANSFER_STORE_ANNOUNCE, document, &used) < 0) return -1;
    if (document->chunk_count == 0U || document->chunk_count > (size - used) / DESCRIPTOR_SIZE || size - used != document->chunk_count * DESCRIPTOR_SIZE || document->chunk_count > SIZE_MAX / sizeof(**chunks)) { errno = EBADMSG; return -1; }
    MetadataChunk *list = calloc((size_t)document->chunk_count, sizeof(*list));
    if (list == NULL) return -1;
    for (uint64_t i = 0U; i < document->chunk_count; ++i) decode_descriptor(payload + used + (size_t)i * DESCRIPTOR_SIZE, &list[i]);
    *chunks = list;
    return 0;
}
