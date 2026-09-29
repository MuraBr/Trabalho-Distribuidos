#ifndef TRANSFER_PROTOCOL_H
#define TRANSFER_PROTOCOL_H

#include "compression.h"
#include "metadata.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    TRANSFER_STORE_BEGIN = 1,
    TRANSFER_STORE_CHUNK = 2,
    TRANSFER_STORE_COMMIT = 3,
    TRANSFER_STORE_ANNOUNCE = 4
} TransferStoreOperation;

typedef enum
{
    TRANSFER_DOWNLOAD_METADATA = 1,
    TRANSFER_DOWNLOAD_CHUNK = 2
} TransferDownloadOperation;

typedef enum
{
    TRANSFER_SELECTOR_OBJECT_ID = 1,
    TRANSFER_SELECTOR_NAME = 2
} TransferSelectorType;

typedef enum
{
    TRANSFER_CREATED = 0,
    TRANSFER_QUEUED = 1,
    TRANSFER_STARTED = 2,
    TRANSFER_TRANSFERRING = 3,
    TRANSFER_VERIFYING = 4,
    TRANSFER_FINISHED = 5,
    TRANSFER_REPLICATED = 6
} TransferState;

typedef struct
{
    ObjectID id;
    char name[METADATA_NAME_SIZE];
    uint64_t file_size;
    uint64_t chunk_count;
    uint8_t compression;
} TransferDocument;

typedef struct
{
    ObjectID id;
    uint64_t index;
    uint64_t offset;
    uint32_t raw_size;
    uint32_t compressed_size;
    uint8_t hash[OBJECT_ID_SIZE];
    const uint8_t *data;
} TransferChunk;

typedef struct
{
    NodeID node_id;
    char ip[NODE_ADDRESS_SIZE];
    uint16_t port;
} TransferEndpoint;

typedef struct
{
    size_t peer_count;
    TransferEndpoint *peers;
} TransferChunkLocations;

typedef struct
{
    TransferDocument document;
    TransferChunkLocations *chunks;
} TransferLookupResult;

void transfer_fill_transaction_id(uint8_t output[TRANSACTION_ID_SIZE], const uint8_t source_node[NODE_ID_SIZE]);
int transfer_encode_document(const TransferDocument *document, uint8_t **output, uint32_t *output_size, uint8_t operation);
int transfer_decode_document(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferDocument *document);
int transfer_encode_chunk(const TransferChunk *chunk, uint8_t operation, uint8_t **output, uint32_t *output_size);
int transfer_decode_chunk(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, TransferChunk *chunk);
int transfer_encode_object_operation(uint8_t operation, const ObjectID *id, uint8_t **output, uint32_t *output_size);
int transfer_decode_object_operation(const uint8_t *payload, size_t payload_size, uint8_t expected_operation, ObjectID *id);
int transfer_encode_lookup_request(const char *selector, uint8_t **output, uint32_t *output_size);
int transfer_decode_lookup_request(const uint8_t *payload, size_t payload_size, TransferSelectorType *type, ObjectID *id, char name[METADATA_NAME_SIZE]);
int transfer_encode_lookup_result(const TransferLookupResult *result, uint8_t **output, uint32_t *output_size);
int transfer_decode_lookup_result(const uint8_t *payload, size_t payload_size, TransferLookupResult *result);
void transfer_lookup_result_free(TransferLookupResult *result);
int transfer_encode_chunk_request(const ObjectID *id, uint64_t index, uint8_t **output, uint32_t *output_size);
int transfer_decode_chunk_request(const uint8_t *payload, size_t payload_size, ObjectID *id, uint64_t *index);

#endif
