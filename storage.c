#define _POSIX_C_SOURCE 200809L
#include "storage.h"

#include "compression.h"
#include "content.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MANIFEST_MAGIC "P2PDOC2\0"
#define MANIFEST_MAGIC_SIZE 8U
#define MANIFEST_VERSION 1U

typedef struct
{
    uint64_t index;
    uint64_t offset;
    uint32_t raw_size;
    uint32_t compressed_size;
    uint8_t hash[OBJECT_ID_SIZE];
    int present;
} StoredChunk;

typedef struct StoredDocument
{
    TransferDocument document;
    NodeID owner;
    uint64_t uploaded_at;
    TransferState state;
    StoredChunk *chunks;
    struct StoredDocument *next;
} StoredDocument;

struct Storage
{
    char root[PATH_MAX];
    NodeID owner;
    pthread_mutex_t mutex;
    StoredDocument *documents;
};

static int verify_document(Storage *storage, StoredDocument *stored, int final);

static void put_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}

static void put_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint32_t get_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) | ((uint32_t)source[2] << 8) | source[3];
}

static void put_u64(uint8_t *destination, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        destination[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static uint64_t get_u64(const uint8_t *source)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index)
    {
        value = (value << 8) | source[index];
    }
    return value;
}

static int write_all_fd(int fd, const uint8_t *data, size_t size)
{
    size_t written = 0U;

    while (written < size)
    {
        ssize_t result = write(fd, data + written, size - written);

        if (result > 0)
        {
            written += (size_t)result;
        }
        else if (result < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            if (result == 0)
            {
                errno = EIO;
            }
            return -1;
        }
    }
    return 0;
}

static int read_all_fd(int fd, uint8_t *data, size_t size)
{
    size_t received = 0U;

    while (received < size)
    {
        ssize_t result = read(fd, data + received, size - received);

        if (result > 0)
        {
            received += (size_t)result;
        }
        else if (result < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            errno = result == 0 ? EBADMSG : errno;
            return -1;
        }
    }
    return 0;
}

static int ensure_directory_tree(const char *path)
{
    char copy[PATH_MAX];
    char *cursor;
    size_t length;

    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    length = strnlen(path, sizeof(copy));
    if (length == 0U || length >= sizeof(copy))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(copy, path, length + 1U);
    for (cursor = copy + 1; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            *cursor = '\0';
            if (mkdir(copy, 0700) < 0 && errno != EEXIST)
            {
                return -1;
            }
            *cursor = '/';
        }
    }
    if (mkdir(copy, 0700) < 0 && errno != EEXIST)
    {
        return -1;
    }
    return 0;
}

static int object_hex(const ObjectID *id, char output[OBJECT_ID_HEX_SIZE])
{
    return object_id_to_hex(id, output, OBJECT_ID_HEX_SIZE);
}

static int document_directory(const Storage *storage, const ObjectID *id, int final, char output[PATH_MAX])
{
    char hex[OBJECT_ID_HEX_SIZE];
    int length;

    if (object_hex(id, hex) < 0)
    {
        return -1;
    }
    length = snprintf(output, PATH_MAX, "%s/%s/%s", storage->root, final ? "objects" : "pending", hex);
    if (length < 0 || (size_t)length >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int chunk_path(const Storage *storage, const ObjectID *id, uint64_t index, int final, char output[PATH_MAX])
{
    char directory[PATH_MAX];
    int length;

    if (document_directory(storage, id, final, directory) < 0)
    {
        return -1;
    }
    length = snprintf(output, PATH_MAX, "%s/chunk-%020" PRIu64 ".lz4", directory, index);
    if (length < 0 || (size_t)length >= PATH_MAX)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static StoredDocument *find_document(Storage *storage, const ObjectID *id)
{
    StoredDocument *document;

    for (document = storage->documents; document != NULL; document = document->next)
    {
        if (memcmp(document->document.id.bytes, id->bytes, OBJECT_ID_SIZE) == 0)
        {
            return document;
        }
    }
    return NULL;
}

static int write_manifest(Storage *storage, StoredDocument *stored, int final)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];
    char temporary[PATH_MAX];
    uint8_t scalar[8];
    size_t name_size = strlen(stored->document.name);
    uint64_t index;
    int fd = -1;
    int result = -1;
    int length;

    if (document_directory(storage, &stored->document.id, final, directory) < 0 || ensure_directory_tree(directory) < 0)
    {
        return -1;
    }
    length = snprintf(path, sizeof(path), "%s/manifest.bin", directory);
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    length = snprintf(temporary, sizeof(temporary), "%s/manifest.tmp-%ld", directory, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        return -1;
    }
    if (write_all_fd(fd, (const uint8_t *)MANIFEST_MAGIC, MANIFEST_MAGIC_SIZE) < 0)
    {
        goto cleanup;
    }
    put_u32(scalar, MANIFEST_VERSION);
    if (write_all_fd(fd, scalar, 4U) < 0 || write_all_fd(fd, stored->document.id.bytes, OBJECT_ID_SIZE) < 0)
    {
        goto cleanup;
    }
    put_u64(scalar, stored->document.file_size);
    if (write_all_fd(fd, scalar, 8U) < 0)
    {
        goto cleanup;
    }
    put_u64(scalar, stored->document.chunk_count);
    if (write_all_fd(fd, scalar, 8U) < 0 || write_all_fd(fd, &stored->document.compression, 1U) < 0)
    {
        goto cleanup;
    }
    put_u16(scalar, (uint16_t)name_size);
    if (write_all_fd(fd, scalar, 2U) < 0 || write_all_fd(fd, (const uint8_t *)stored->document.name, name_size) < 0 || write_all_fd(fd, stored->owner.bytes, NODE_ID_SIZE) < 0)
    {
        goto cleanup;
    }
    put_u64(scalar, stored->uploaded_at);
    if (write_all_fd(fd, scalar, 8U) < 0)
    {
        goto cleanup;
    }
    scalar[0] = (uint8_t)stored->state;
    if (write_all_fd(fd, scalar, 1U) < 0)
    {
        goto cleanup;
    }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        StoredChunk *chunk = &stored->chunks[index];

        put_u64(scalar, chunk->index);
        if (write_all_fd(fd, scalar, 8U) < 0)
        {
            goto cleanup;
        }
        put_u64(scalar, chunk->offset);
        if (write_all_fd(fd, scalar, 8U) < 0)
        {
            goto cleanup;
        }
        put_u32(scalar, chunk->raw_size);
        if (write_all_fd(fd, scalar, 4U) < 0)
        {
            goto cleanup;
        }
        put_u32(scalar, chunk->compressed_size);
        if (write_all_fd(fd, scalar, 4U) < 0 || write_all_fd(fd, chunk->hash, OBJECT_ID_SIZE) < 0)
        {
            goto cleanup;
        }
        scalar[0] = (uint8_t)(chunk->present != 0);
        if (write_all_fd(fd, scalar, 1U) < 0)
        {
            goto cleanup;
        }
    }
    if (fsync(fd) < 0)
    {
        goto cleanup;
    }
    if (close(fd) < 0)
    {
        fd = -1;
        goto cleanup;
    }
    fd = -1;
    if (rename(temporary, path) < 0)
    {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (fd >= 0)
    {
        close(fd);
    }
    if (result < 0)
    {
        unlink(temporary);
    }
    return result;
}

static int read_manifest(Storage *storage, const char *path, int final, StoredDocument **output)
{
    uint8_t scalar[8];
    uint8_t magic[MANIFEST_MAGIC_SIZE];
    StoredDocument *stored = NULL;
    uint16_t name_size;
    uint64_t index;
    int fd = -1;
    ssize_t trailing;

    *output = NULL;
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return -1;
    }
    stored = calloc(1U, sizeof(*stored));
    if (stored == NULL)
    {
        goto error;
    }
    if (read_all_fd(fd, magic, sizeof(magic)) < 0 || memcmp(magic, MANIFEST_MAGIC, sizeof(magic)) != 0 || read_all_fd(fd, scalar, 4U) < 0 || get_u32(scalar) != MANIFEST_VERSION || read_all_fd(fd, stored->document.id.bytes, OBJECT_ID_SIZE) < 0 || read_all_fd(fd, scalar, 8U) < 0)
    {
        errno = EBADMSG;
        goto error;
    }
    stored->document.file_size = get_u64(scalar);
    if (read_all_fd(fd, scalar, 8U) < 0)
    {
        goto error;
    }
    stored->document.chunk_count = get_u64(scalar);
    if (stored->document.chunk_count != stored->document.file_size / METADATA_CHUNK_SIZE + (stored->document.file_size % METADATA_CHUNK_SIZE != 0U) || stored->document.chunk_count > SIZE_MAX / sizeof(*stored->chunks) || read_all_fd(fd, &stored->document.compression, 1U) < 0 || stored->document.compression != COMPRESSION_LZ4 || read_all_fd(fd, scalar, 2U) < 0)
    {
        errno = EBADMSG;
        goto error;
    }
    name_size = get_u16(scalar);
    if (name_size == 0U || name_size >= METADATA_NAME_SIZE || read_all_fd(fd, (uint8_t *)stored->document.name, name_size) < 0 || memchr(stored->document.name, '\0', name_size) != NULL || memchr(stored->document.name, '/', name_size) != NULL)
    {
        errno = EBADMSG;
        goto error;
    }
    stored->document.name[name_size] = '\0';
    if (read_all_fd(fd, stored->owner.bytes, NODE_ID_SIZE) < 0 || read_all_fd(fd, scalar, 8U) < 0)
    {
        goto error;
    }
    stored->uploaded_at = get_u64(scalar);
    if (read_all_fd(fd, scalar, 1U) < 0 || scalar[0] > TRANSFER_REPLICATED || (final && scalar[0] != TRANSFER_FINISHED && scalar[0] != TRANSFER_VERIFYING) || (!final && scalar[0] == TRANSFER_FINISHED))
    {
        errno = EBADMSG;
        goto error;
    }
    stored->state = (TransferState)scalar[0];
    if (stored->document.chunk_count != 0U)
    {
        stored->chunks = calloc((size_t)stored->document.chunk_count, sizeof(*stored->chunks));
        if (stored->chunks == NULL)
        {
            goto error;
        }
    }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        StoredChunk *chunk = &stored->chunks[index];
        char chunk_file[PATH_MAX];

        if (read_all_fd(fd, scalar, 8U) < 0)
        {
            goto error;
        }
        chunk->index = get_u64(scalar);
        if (read_all_fd(fd, scalar, 8U) < 0)
        {
            goto error;
        }
        chunk->offset = get_u64(scalar);
        if (read_all_fd(fd, scalar, 4U) < 0)
        {
            goto error;
        }
        chunk->raw_size = get_u32(scalar);
        if (read_all_fd(fd, scalar, 4U) < 0)
        {
            goto error;
        }
        chunk->compressed_size = get_u32(scalar);
        if (read_all_fd(fd, chunk->hash, OBJECT_ID_SIZE) < 0 || read_all_fd(fd, scalar, 1U) < 0)
        {
            goto error;
        }
        chunk->present = scalar[0] != 0U;
        if (scalar[0] > 1U || (final && !chunk->present))
        {
            errno = EBADMSG;
            goto error;
        }
        if (chunk->present)
        {
            uint64_t remaining = stored->document.file_size - index * METADATA_CHUNK_SIZE;
            uint32_t expected_size = (uint32_t)(remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining);
            size_t bound;
            struct stat information;

            if (chunk->index != index || chunk->offset != index * METADATA_CHUNK_SIZE || chunk->raw_size != expected_size || compression_lz4_bound(chunk->raw_size, &bound) < 0 || chunk->compressed_size == 0U || chunk->compressed_size > bound || chunk_path(storage, &stored->document.id, index, final, chunk_file) < 0 || stat(chunk_file, &information) < 0 || !S_ISREG(information.st_mode) || information.st_size != (off_t)chunk->compressed_size)
            {
                errno = EBADMSG;
                goto error;
            }
        }
        else if (chunk->index != 0U || chunk->offset != 0U || chunk->raw_size != 0U || chunk->compressed_size != 0U)
        {
            errno = EBADMSG;
            goto error;
        }
    }
    trailing = read(fd, scalar, 1U);
    if (trailing != 0)
    {
        if (trailing > 0)
        {
            errno = EBADMSG;
        }
        goto error;
    }
    if (final && verify_document(storage, stored, 1) < 0)
    {
        goto error;
    }
    if (close(fd) < 0)
    {
        fd = -1;
        goto error;
    }
    fd = -1;
    *output = stored;
    return 0;

error:
    if (fd >= 0)
    {
        close(fd);
    }
    if (stored != NULL)
    {
        free(stored->chunks);
        free(stored);
    }
    return -1;
}

static int load_documents(Storage *storage, int final)
{
    char path[PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    int length;

    length = snprintf(path, sizeof(path), "%s/%s", storage->root, final ? "objects" : "pending");
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    directory = opendir(path);
    if (directory == NULL)
    {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL)
    {
        char manifest[PATH_MAX];
        StoredDocument *stored;

        if (entry->d_name[0] == '.')
        {
            continue;
        }
        length = snprintf(manifest, sizeof(manifest), "%s/%s/manifest.bin", path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(manifest))
        {
            continue;
        }
        if (read_manifest(storage, manifest, final, &stored) == 0)
        {
            char expected[OBJECT_ID_HEX_SIZE];

            if (object_hex(&stored->document.id, expected) < 0 || strcmp(entry->d_name, expected) != 0 || find_document(storage, &stored->document.id) != NULL)
            {
                free(stored->chunks);
                free(stored);
                continue;
            }
            if (final)
            {
                int recover = stored->state == TRANSFER_VERIFYING;

                stored->state = TRANSFER_FINISHED;
                if (recover && write_manifest(storage, stored, 1) < 0)
                {
                    free(stored->chunks);
                    free(stored);
                    (void)closedir(directory);
                    return -1;
                }
            }
            stored->next = storage->documents;
            storage->documents = stored;
        }
    }
    return closedir(directory);
}

int storage_create(const char *root, const NodeID *owner, Storage **output)
{
    Storage *storage;
    char path[PATH_MAX];
    int error;

    if (root == NULL || owner == NULL || output == NULL || strnlen(root, PATH_MAX) >= PATH_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    storage = calloc(1U, sizeof(*storage));
    if (storage == NULL)
    {
        return -1;
    }
    strcpy(storage->root, root);
    storage->owner = *owner;
    if (ensure_directory_tree(storage->root) < 0 || snprintf(path, sizeof(path), "%s/pending", storage->root) < 0 || ensure_directory_tree(path) < 0 || snprintf(path, sizeof(path), "%s/objects", storage->root) < 0 || ensure_directory_tree(path) < 0)
    {
        free(storage);
        return -1;
    }
    error = pthread_mutex_init(&storage->mutex, NULL);
    if (error != 0)
    {
        free(storage);
        errno = error;
        return -1;
    }
    if (load_documents(storage, 1) < 0 || load_documents(storage, 0) < 0)
    {
        storage_destroy(storage);
        return -1;
    }
    *output = storage;
    return 0;
}

void storage_destroy(Storage *storage)
{
    StoredDocument *document;

    if (storage == NULL)
    {
        return;
    }
    document = storage->documents;
    while (document != NULL)
    {
        StoredDocument *next = document->next;

        free(document->chunks);
        free(document);
        document = next;
    }
    pthread_mutex_destroy(&storage->mutex);
    free(storage);
}

int storage_begin(Storage *storage, const TransferDocument *document)
{
    StoredDocument *stored;
    char directory[PATH_MAX];
    int error;

    if (storage == NULL || document == NULL || document->compression != COMPRESSION_LZ4 || document->chunk_count == 0U || document->chunk_count > SIZE_MAX / sizeof(*stored->chunks))
    {
        errno = EINVAL;
        return -1;
    }
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, &document->id);
    if (stored != NULL)
    {
        int matches = stored->document.file_size == document->file_size && stored->document.chunk_count == document->chunk_count;

        pthread_mutex_unlock(&storage->mutex);
        if (!matches)
        {
            errno = EEXIST;
            return -1;
        }
        return 0;
    }
    stored = calloc(1U, sizeof(*stored));
    if (stored == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    stored->chunks = calloc((size_t)document->chunk_count, sizeof(*stored->chunks));
    if (stored->chunks == NULL)
    {
        free(stored);
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    stored->document = *document;
    stored->owner = storage->owner;
    stored->uploaded_at = (uint64_t)time(NULL);
    stored->state = TRANSFER_CREATED;
    stored->next = storage->documents;
    storage->documents = stored;
    if (document_directory(storage, &document->id, 0, directory) < 0 || ensure_directory_tree(directory) < 0 || write_manifest(storage, stored, 0) < 0)
    {
        storage->documents = stored->next;
        free(stored->chunks);
        free(stored);
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    pthread_mutex_unlock(&storage->mutex);
    return 0;
}

int storage_put_chunk(Storage *storage, const TransferChunk *chunk)
{
    StoredDocument *stored;
    StoredChunk *record;
    uint8_t calculated[OBJECT_ID_SIZE];
    uint8_t *raw = NULL;
    char final_path[PATH_MAX];
    char temporary[PATH_MAX];
    uint64_t expected_offset;
    uint64_t remaining;
    uint32_t expected_size;
    size_t bound;
    int fd = -1;
    int error;
    int length;

    if (storage == NULL || chunk == NULL || chunk->data == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (compression_lz4_bound(chunk->raw_size, &bound) < 0 || chunk->compressed_size > bound || compression_lz4_decompress(chunk->data, chunk->compressed_size, chunk->raw_size, &raw) < 0 || content_sha256(raw, chunk->raw_size, calculated) < 0 || memcmp(calculated, chunk->hash, OBJECT_ID_SIZE) != 0)
    {
        free(raw);
        errno = EBADMSG;
        return -1;
    }
    free(raw);
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, &chunk->id);
    if (stored == NULL || chunk->index >= stored->document.chunk_count || chunk->index > UINT64_MAX / METADATA_CHUNK_SIZE)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = stored == NULL ? ENOENT : EINVAL;
        return -1;
    }
    expected_offset = chunk->index * METADATA_CHUNK_SIZE;
    remaining = stored->document.file_size - expected_offset;
    expected_size = (uint32_t)(remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining);
    if (chunk->offset != expected_offset || chunk->raw_size != expected_size)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = EINVAL;
        return -1;
    }
    record = &stored->chunks[chunk->index];
    if (stored->state == TRANSFER_FINISHED)
    {
        int matches = record->present && record->raw_size == chunk->raw_size && record->compressed_size == chunk->compressed_size && memcmp(record->hash, chunk->hash, OBJECT_ID_SIZE) == 0;

        pthread_mutex_unlock(&storage->mutex);
        if (!matches)
        {
            errno = EEXIST;
            return -1;
        }
        return 0;
    }
    if (record->present)
    {
        int matches = record->raw_size == chunk->raw_size && record->compressed_size == chunk->compressed_size && memcmp(record->hash, chunk->hash, OBJECT_ID_SIZE) == 0;

        pthread_mutex_unlock(&storage->mutex);
        if (!matches)
        {
            errno = EEXIST;
            return -1;
        }
        return 0;
    }
    if (chunk_path(storage, &chunk->id, chunk->index, 0, final_path) < 0)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    length = snprintf(temporary, sizeof(temporary), "%s.tmp-%ld", final_path, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary))
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write_all_fd(fd, chunk->data, chunk->compressed_size) < 0 || fsync(fd) < 0)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        unlink(temporary);
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    if (close(fd) < 0)
    {
        fd = -1;
        unlink(temporary);
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    fd = -1;
    if (rename(temporary, final_path) < 0)
    {
        unlink(temporary);
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    record->index = chunk->index;
    record->offset = chunk->offset;
    record->raw_size = chunk->raw_size;
    record->compressed_size = chunk->compressed_size;
    memcpy(record->hash, chunk->hash, OBJECT_ID_SIZE);
    record->present = 1;
    stored->state = TRANSFER_TRANSFERRING;
    if (write_manifest(storage, stored, 0) < 0)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    pthread_mutex_unlock(&storage->mutex);
    return 0;
}

static int verify_document(Storage *storage, StoredDocument *stored, int final)
{
    char verified_path[PATH_MAX];
    ObjectID actual;
    uint64_t actual_size;
    uint64_t index;
    int fd;
    int length;

    length = snprintf(verified_path, sizeof(verified_path), "%s/pending/verify-%ld.pdf", storage->root, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(verified_path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(verified_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        return -1;
    }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        StoredChunk *record = &stored->chunks[index];
        char path[PATH_MAX];
        uint8_t *compressed = NULL;
        uint8_t *raw = NULL;
        uint8_t digest[OBJECT_ID_SIZE];
        int chunk_fd = -1;

        if (!record->present || chunk_path(storage, &stored->document.id, index, final, path) < 0)
        {
            errno = ENODATA;
            goto error;
        }
        compressed = malloc(record->compressed_size);
        if (compressed == NULL)
        {
            goto error;
        }
        chunk_fd = open(path, O_RDONLY);
        if (chunk_fd < 0 || read_all_fd(chunk_fd, compressed, record->compressed_size) < 0)
        {
            if (chunk_fd >= 0)
            {
                close(chunk_fd);
            }
            free(compressed);
            goto error;
        }
        if (close(chunk_fd) < 0)
        {
            chunk_fd = -1;
            free(compressed);
            goto error;
        }
        chunk_fd = -1;
        if (compression_lz4_decompress(compressed, record->compressed_size, record->raw_size, &raw) < 0 || content_sha256(raw, record->raw_size, digest) < 0 || memcmp(digest, record->hash, OBJECT_ID_SIZE) != 0 || write_all_fd(fd, raw, record->raw_size) < 0)
        {
            free(compressed);
            free(raw);
            errno = EBADMSG;
            goto error;
        }
        free(compressed);
        free(raw);
    }
    if (fsync(fd) < 0)
    {
        goto error;
    }
    if (close(fd) < 0)
    {
        fd = -1;
        goto error_closed;
    }
    fd = -1;
    if (object_id_file(verified_path, &actual, &actual_size) < 0 || actual_size != stored->document.file_size || memcmp(actual.bytes, stored->document.id.bytes, OBJECT_ID_SIZE) != 0)
    {
        errno = EBADMSG;
        goto error_closed;
    }
    unlink(verified_path);
    return 0;

error:
    close(fd);
error_closed:
    unlink(verified_path);
    return -1;
}

int storage_commit(Storage *storage, const ObjectID *id, TransferDocument *document)
{
    StoredDocument *stored;
    char pending[PATH_MAX];
    char final[PATH_MAX];
    uint64_t index;
    int error;

    if (storage == NULL || id == NULL || document == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, id);
    if (stored == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = ENOENT;
        return -1;
    }
    if (stored->state == TRANSFER_FINISHED)
    {
        *document = stored->document;
        pthread_mutex_unlock(&storage->mutex);
        return 0;
    }
    for (index = 0U; index < stored->document.chunk_count; ++index)
    {
        if (!stored->chunks[index].present)
        {
            pthread_mutex_unlock(&storage->mutex);
            errno = ENODATA;
            return -1;
        }
    }
    stored->state = TRANSFER_VERIFYING;
    if (verify_document(storage, stored, 0) < 0 || write_manifest(storage, stored, 0) < 0 || document_directory(storage, id, 0, pending) < 0 || document_directory(storage, id, 1, final) < 0)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    if (rename(pending, final) < 0)
    {
        if (errno != EEXIST)
        {
            pthread_mutex_unlock(&storage->mutex);
            return -1;
        }
    }
    stored->state = TRANSFER_FINISHED;
    if (write_manifest(storage, stored, 1) < 0)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    *document = stored->document;
    pthread_mutex_unlock(&storage->mutex);
    return 0;
}

int storage_find(Storage *storage, TransferSelectorType type, const ObjectID *id, const char *name, TransferDocument *document)
{
    StoredDocument *current;
    StoredDocument *found = NULL;
    int error;

    if (storage == NULL || document == NULL || (type == TRANSFER_SELECTOR_OBJECT_ID && id == NULL) || (type == TRANSFER_SELECTOR_NAME && name == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    for (current = storage->documents; current != NULL; current = current->next)
    {
        int matches = type == TRANSFER_SELECTOR_OBJECT_ID ? memcmp(current->document.id.bytes, id->bytes, OBJECT_ID_SIZE) == 0 : strcmp(current->document.name, name) == 0;

        if (matches && current->state == TRANSFER_FINISHED)
        {
            if (found != NULL && memcmp(found->document.id.bytes, current->document.id.bytes, OBJECT_ID_SIZE) != 0)
            {
                pthread_mutex_unlock(&storage->mutex);
                errno = ENOTUNIQ;
                return -1;
            }
            found = current;
        }
    }
    if (found == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = ENOENT;
        return -1;
    }
    *document = found->document;
    pthread_mutex_unlock(&storage->mutex);
    return 0;
}

int storage_read_chunk(Storage *storage, const ObjectID *id, uint64_t index, TransferChunk *chunk, uint8_t **owned_data)
{
    StoredDocument *stored;
    StoredChunk record;
    char path[PATH_MAX];
    uint8_t *data;
    int fd;
    int error;

    if (storage == NULL || id == NULL || chunk == NULL || owned_data == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *owned_data = NULL;
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    stored = find_document(storage, id);
    if (stored == NULL || stored->state != TRANSFER_FINISHED || index >= stored->document.chunk_count)
    {
        pthread_mutex_unlock(&storage->mutex);
        errno = ENOENT;
        return -1;
    }
    record = stored->chunks[index];
    if (chunk_path(storage, id, index, 1, path) < 0)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    pthread_mutex_unlock(&storage->mutex);
    data = malloc(record.compressed_size);
    if (data == NULL)
    {
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0 || read_all_fd(fd, data, record.compressed_size) < 0)
    {
        if (fd >= 0)
        {
            close(fd);
        }
        free(data);
        return -1;
    }
    if (close(fd) < 0)
    {
        free(data);
        return -1;
    }
    memset(chunk, 0, sizeof(*chunk));
    chunk->id = *id;
    chunk->index = record.index;
    chunk->offset = record.offset;
    chunk->raw_size = record.raw_size;
    chunk->compressed_size = record.compressed_size;
    memcpy(chunk->hash, record.hash, OBJECT_ID_SIZE);
    chunk->data = data;
    *owned_data = data;
    return 0;
}

int storage_list(Storage *storage, TransferDocument **documents, size_t *count)
{
    StoredDocument *current;
    TransferDocument *list;
    size_t total = 0U;
    size_t index = 0U;
    int error;

    if (storage == NULL || documents == NULL || count == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *documents = NULL;
    *count = 0U;
    error = pthread_mutex_lock(&storage->mutex);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    for (current = storage->documents; current != NULL; current = current->next)
    {
        if (current->state == TRANSFER_FINISHED)
        {
            ++total;
        }
    }
    list = total == 0U ? NULL : malloc(total * sizeof(*list));
    if (total != 0U && list == NULL)
    {
        pthread_mutex_unlock(&storage->mutex);
        return -1;
    }
    if (total == 0U)
    {
        pthread_mutex_unlock(&storage->mutex);
        return 0;
    }
    for (current = storage->documents; current != NULL; current = current->next)
    {
        if (current->state == TRANSFER_FINISHED)
        {
            list[index++] = current->document;
        }
    }
    pthread_mutex_unlock(&storage->mutex);
    *documents = list;
    *count = total;
    return 0;
}
