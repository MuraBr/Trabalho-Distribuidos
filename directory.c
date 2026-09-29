#include "directory.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct Directory { MetadataStore *metadata; SuperPeer *superpeer; };

int directory_create(MetadataStore *metadata, SuperPeer *superpeer, Directory **output)
{
    if (metadata == NULL || superpeer == NULL || output == NULL) { errno = EINVAL; return -1; }
    *output = calloc(1U, sizeof(**output));
    if (*output == NULL) return -1;
    (*output)->metadata = metadata;
    (*output)->superpeer = superpeer;
    return 0;
}

void directory_destroy(Directory *directory) { free(directory); }

/* O índice de nomes e o anúncio completo pertencem à mesma hash table. */
int directory_announce(Directory *directory, const TransferDocument *document, const MetadataChunk *chunks, const NodeID *owner)
{
    if (directory == NULL || document == NULL || owner == NULL || document->compression != COMPRESSION_LZ4) { errno = EINVAL; return -1; }
    size_t length = strlen(document->name);
    if (length < 4U || strcasecmp(document->name + length - 4U, ".pdf") != 0) { errno = EINVAL; return -1; }
    MetadataDocument metadata = {.id = document->id, .file_size = document->file_size, .chunk_count = document->chunk_count, .version = 1U, .owner = *owner, .compression = document->compression};
    memcpy(metadata.name, document->name, sizeof(metadata.name));
    return metadata_announce(directory->metadata, &metadata, chunks, owner);
}

int directory_lookup(Directory *directory, TransferSelectorType type, const ObjectID *id, const char *name, TransferLookupResult *result)
{
    ObjectID selected;
    MetadataDocument metadata_document;
    uint64_t chunk_index;

    if (directory == NULL || result == NULL || (type == TRANSFER_SELECTOR_OBJECT_ID && id == NULL) || (type == TRANSFER_SELECTOR_NAME && name == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (type == TRANSFER_SELECTOR_OBJECT_ID)
    {
        selected = *id;
    }
    else if (metadata_find_name(directory->metadata, name, &selected) < 0)
    {
        return -1;
    }
    if (metadata_find_document(directory->metadata, &selected, &metadata_document) < 0 || metadata_document.chunk_count > SIZE_MAX / sizeof(*result->chunks))
    {
        return -1;
    }
    result->document.id = metadata_document.id;
    strcpy(result->document.name, metadata_document.name);
    result->document.file_size = metadata_document.file_size;
    result->document.chunk_count = metadata_document.chunk_count;
    result->document.compression = COMPRESSION_LZ4;
    if (metadata_document.chunk_count != 0U)
    {
        result->chunks = calloc((size_t)metadata_document.chunk_count, sizeof(*result->chunks));
        if (result->chunks == NULL)
        {
            return -1;
        }
    }
    for (chunk_index = 0U; chunk_index < metadata_document.chunk_count; ++chunk_index)
    {
        if (metadata_chunk_descriptor(directory->metadata, &selected, chunk_index, &result->chunks[chunk_index].descriptor) < 0) { transfer_lookup_result_free(result); return -1; }
        NodeID *owners = NULL;
        size_t owner_count = 0U;
        size_t owner_index;
        size_t valid_count = 0U;
        TransferEndpoint *endpoints = NULL;

        if (metadata_chunk_peers(directory->metadata, &selected, chunk_index, &owners, &owner_count) < 0)
        {
            transfer_lookup_result_free(result);
            return -1;
        }
        if (owner_count != 0U)
        {
            endpoints = calloc(owner_count, sizeof(*endpoints));
            if (endpoints == NULL)
            {
                free(owners);
                transfer_lookup_result_free(result);
                return -1;
            }
        }
        for (owner_index = 0U; owner_index < owner_count; ++owner_index)
        {
            SuperPeerMember member;

            if (superpeer_find_member(directory->superpeer, &owners[owner_index], &member) == 0)
            {
                TransferEndpoint *endpoint = &endpoints[valid_count++];

                endpoint->node_id = owners[owner_index];
                strcpy(endpoint->ip, member.node.config.ip);
                endpoint->port = member.node.config.port;
            }
        }
        free(owners);
        result->chunks[chunk_index].peers = endpoints;
        result->chunks[chunk_index].peer_count = valid_count;
        if (valid_count == 0U)
        {
            transfer_lookup_result_free(result);
            errno = ENODATA;
            return -1;
        }
    }
    return 0;
}
