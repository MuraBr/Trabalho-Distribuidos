#ifndef TRANSFER_TYPES_H
#define TRANSFER_TYPES_H
#include "compression.h"
#include "metadata.h"
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
    MetadataChunk descriptor;
    size_t peer_count;
    TransferEndpoint *peers;
} TransferChunkLocations;

typedef struct
{
    TransferDocument document;
    TransferChunkLocations *chunks;
} TransferLookupResult;
#endif
