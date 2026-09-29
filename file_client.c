#define _POSIX_C_SOURCE 200809L
#include "file_client.h"

#include "compression.h"
#include "content.h"
#include "metadata.h"
#include "network.h"
#include "rpc.h"
#include "transfer_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_MAX_WORKERS 8U
#define MAX_CONFIGURED_WORKERS 32U
#define PART_PATH_EXTRA 6U

typedef struct
{
    int input_fd;
    const char *host;
    uint16_t port;
    TransferDocument document;
    uint8_t *hashes;
    uint64_t next_index;
    uint64_t compressed_total;
    TransferState state;
    int failed;
    int saved_errno;
    pthread_mutex_t mutex;
} UploadWork;

typedef struct
{
    int output_fd;
    const TransferLookupResult *lookup;
    uint64_t next_index;
    int failed;
    int saved_errno;
    uint64_t compressed_total;
    TransferState state;
    pthread_mutex_t mutex;
} DownloadWork;

static size_t transfer_worker_count(uint64_t chunk_count)
{
    const char *configured = getenv("PEER_TRANSFER_THREADS");
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long wanted = cpu_count > 0L ? (unsigned long)cpu_count : 1UL;

    if (wanted > DEFAULT_MAX_WORKERS)
    {
        wanted = DEFAULT_MAX_WORKERS;
    }
    if (configured != NULL && configured[0] != '\0')
    {
        char *end = NULL;
        unsigned long value;

        errno = 0;
        value = strtoul(configured, &end, 10);
        if (errno == 0 && end != configured && *end == '\0' && value >= 1UL && value <= MAX_CONFIGURED_WORKERS)
        {
            wanted = value;
        }
    }
    if ((uint64_t)wanted > chunk_count)
    {
        wanted = (unsigned long)chunk_count;
    }
    return wanted == 0UL ? 1U : (size_t)wanted;
}

static int request_expect(const char *host, uint16_t port, Message_Type type, const uint8_t *payload, uint32_t payload_size, Message_Type expected, Message *response)
{
    if (rpc_call(host, port, NULL, NULL, type, payload, payload_size, response) < 0)
    {
        return -1;
    }
    if (response->header.message_type != (uint8_t)expected)
    {
        message_free(response);
        errno = EREMOTEIO;
        return -1;
    }
    return 0;
}

static int pread_all(int fd, uint8_t *buffer, size_t size, uint64_t offset)
{
    size_t done = 0U;

    if (offset > (uint64_t)INT64_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    while (done < size)
    {
        ssize_t count = pread(fd, buffer + done, size - done, (off_t)(offset + done));

        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            if (count == 0)
            {
                errno = EIO;
            }
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

static int pwrite_all(int fd, const uint8_t *buffer, size_t size, uint64_t offset)
{
    size_t done = 0U;

    if (offset > (uint64_t)INT64_MAX)
    {
        errno = EOVERFLOW;
        return -1;
    }
    while (done < size)
    {
        ssize_t count = pwrite(fd, buffer + done, size - done, (off_t)(offset + done));

        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

static void upload_fail(UploadWork *work, int error)
{
    (void)pthread_mutex_lock(&work->mutex);
    if (!work->failed)
    {
        work->failed = 1;
        work->saved_errno = error == 0 ? EIO : error;
    }
    (void)pthread_mutex_unlock(&work->mutex);
}

static void *upload_worker(void *argument)
{
    UploadWork *work = argument;

    for (;;)
    {
        uint64_t index;
        uint64_t offset;
        uint64_t remaining;
        size_t raw_size;
        uint8_t *raw = NULL;
        uint8_t *compressed = NULL;
        size_t compressed_size = 0U;
        uint8_t *payload = NULL;
        uint32_t payload_size = 0U;
        TransferChunk chunk;
        Message response;

        (void)pthread_mutex_lock(&work->mutex);
        if (work->failed || work->next_index >= work->document.chunk_count)
        {
            (void)pthread_mutex_unlock(&work->mutex);
            break;
        }
        index = work->next_index++;
        work->state = TRANSFER_STARTED;
        (void)pthread_mutex_unlock(&work->mutex);
        (void)pthread_mutex_lock(&work->mutex);
        work->state = TRANSFER_TRANSFERRING;
        (void)pthread_mutex_unlock(&work->mutex);
        offset = index * METADATA_CHUNK_SIZE;
        remaining = work->document.file_size - offset;
        raw_size = remaining > METADATA_CHUNK_SIZE ? (size_t)METADATA_CHUNK_SIZE : (size_t)remaining;
        raw = malloc(raw_size);
        if (raw == NULL || pread_all(work->input_fd, raw, raw_size, offset) < 0)
        {
            upload_fail(work, errno);
            free(raw);
            break;
        }
        memset(&chunk, 0, sizeof(chunk));
        chunk.id = work->document.id;
        chunk.index = index;
        chunk.offset = offset;
        chunk.raw_size = (uint32_t)raw_size;
        if (content_sha256(raw, raw_size, chunk.hash) < 0 || compression_lz4_compress(raw, raw_size, &compressed, &compressed_size) < 0 || compressed_size > UINT32_MAX)
        {
            upload_fail(work, errno);
            free(raw);
            free(compressed);
            break;
        }
        chunk.compressed_size = (uint32_t)compressed_size;
        chunk.data = compressed;
        memcpy(work->hashes + (size_t)index * OBJECT_ID_SIZE, chunk.hash, OBJECT_ID_SIZE);
        if (transfer_encode_chunk(&chunk, TRANSFER_STORE_CHUNK, &payload, &payload_size) < 0 || request_expect(work->host, work->port, M_STORE, payload, payload_size, M_ACK, &response) < 0)
        {
            upload_fail(work, errno);
            free(payload);
            free(compressed);
            free(raw);
            break;
        }
        message_free(&response);
        (void)pthread_mutex_lock(&work->mutex);
        work->compressed_total += compressed_size;
        (void)pthread_mutex_unlock(&work->mutex);
        free(payload);
        free(compressed);
        free(raw);
    }
    return NULL;
}

static int execute_upload_workers(UploadWork *work, size_t worker_count)
{
    pthread_t *threads = calloc(worker_count, sizeof(*threads));
    size_t started = 0U;
    size_t index;

    if (threads == NULL)
    {
        return -1;
    }
    for (index = 0U; index < worker_count; ++index)
    {
        int error = pthread_create(&threads[index], NULL, upload_worker, work);

        if (error != 0)
        {
            upload_fail(work, error);
            break;
        }
        ++started;
    }
    for (index = 0U; index < started; ++index)
    {
        (void)pthread_join(threads[index], NULL);
    }
    free(threads);
    if (work->failed)
    {
        errno = work->saved_errno;
        return -1;
    }
    return 0;
}

int file_client_upload(const char *path, const char *peer_host, uint16_t peer_port)
{
    TransferDocument document;
    UploadWork work;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    Message response;
    char object_hex[OBJECT_ID_HEX_SIZE];
    int input_fd = -1;
    size_t worker_count;
    struct timespec started;
    struct timespec finished;
    double elapsed;
    int status = -1;

    memset(&document, 0, sizeof(document));
    memset(&work, 0, sizeof(work));
    if (content_validate_pdf(path) < 0 || object_id_file(path, &document.id, &document.file_size) < 0 || content_basename(path, document.name) < 0 || document.file_size == 0U)
    {
        return -1;
    }
    document.chunk_count = document.file_size / METADATA_CHUNK_SIZE + (document.file_size % METADATA_CHUNK_SIZE != 0U);
    document.compression = COMPRESSION_LZ4;
    if (document.chunk_count > SIZE_MAX / OBJECT_ID_SIZE)
    {
        errno = EOVERFLOW;
        return -1;
    }
    input_fd = open(path, O_RDONLY);
    work.hashes = calloc((size_t)document.chunk_count, OBJECT_ID_SIZE);
    if (input_fd < 0 || work.hashes == NULL)
    {
        goto cleanup;
    }
    work.input_fd = input_fd;
    work.host = peer_host;
    work.port = peer_port;
    work.document = document;
    work.state = TRANSFER_CREATED;
    if (pthread_mutex_init(&work.mutex, NULL) != 0)
    {
        errno = EBUSY;
        goto cleanup;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    if (transfer_encode_document(&document, &payload, &payload_size, TRANSFER_STORE_BEGIN) < 0 || request_expect(peer_host, peer_port, M_STORE, payload, payload_size, M_ACK, &response) < 0)
    {
        goto mutex_cleanup;
    }
    message_free(&response);
    free(payload);
    payload = NULL;
    worker_count = transfer_worker_count(document.chunk_count);
    work.state = TRANSFER_QUEUED;
    printf("Transfer workers: %zu\n", worker_count);
    if (execute_upload_workers(&work, worker_count) < 0)
    {
        goto mutex_cleanup;
    }
    work.state = TRANSFER_VERIFYING;
    if (transfer_encode_object_operation(TRANSFER_STORE_COMMIT, &document.id, &payload, &payload_size) < 0 || request_expect(peer_host, peer_port, M_STORE, payload, payload_size, M_ACK, &response) < 0)
    {
        goto mutex_cleanup;
    }
    message_free(&response);
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    elapsed = (double)(finished.tv_sec - started.tv_sec) + (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    if (object_id_to_hex(&document.id, object_hex, sizeof(object_hex)) < 0)
    {
        goto mutex_cleanup;
    }
    work.state = TRANSFER_FINISHED;
    printf("File: %s\nSize: %" PRIu64 "\nObjectID: %s\nChunks: %" PRIu64 "\n", document.name, document.file_size, object_hex, document.chunk_count);
    for (uint64_t index = 0U; index < document.chunk_count; ++index)
    {
        printf("Chunk %" PRIu64 ": ", index);
        for (size_t byte = 0U; byte < OBJECT_ID_SIZE; ++byte)
        {
            printf("%02x", (unsigned)work.hashes[(size_t)index * OBJECT_ID_SIZE + byte]);
        }
        printf("\n");
    }
    printf("Compression: LZ4\nOriginal bytes: %" PRIu64 "\nCompressed bytes: %" PRIu64 "\nDuration: %.3f s\nThroughput: %.2f MiB/s\nState: FINISHED\nUpload completed\n", document.file_size, work.compressed_total, elapsed, elapsed > 0.0 ? ((double)document.file_size / 1048576.0) / elapsed : 0.0);
    status = 0;

mutex_cleanup:
    free(payload);
    (void)pthread_mutex_destroy(&work.mutex);
cleanup:
    if (input_fd >= 0)
    {
        (void)close(input_fd);
    }
    free(work.hashes);
    return status;
}

static void download_fail(DownloadWork *work, int error)
{
    (void)pthread_mutex_lock(&work->mutex);
    if (!work->failed)
    {
        work->failed = 1;
        work->saved_errno = error == 0 ? EIO : error;
    }
    (void)pthread_mutex_unlock(&work->mutex);
}

static int download_one(DownloadWork *work, uint64_t index)
{
    const TransferLookupResult *lookup = work->lookup;
    const TransferChunkLocations *locations = &lookup->chunks[index];
    uint8_t *request = NULL;
    uint32_t request_size = 0U;
    size_t endpoint_index;

    if (transfer_encode_chunk_request(&lookup->document.id, index, &request, &request_size) < 0)
    {
        return -1;
    }
    for (endpoint_index = 0U; endpoint_index < locations->peer_count; ++endpoint_index)
    {
        const TransferEndpoint *endpoint = &locations->peers[endpoint_index];
        Message response;
        TransferChunk chunk;
        uint8_t *raw = NULL;
        uint8_t hash[OBJECT_ID_SIZE];
        uint64_t expected_offset = index * METADATA_CHUNK_SIZE;
        uint64_t remaining = lookup->document.file_size - expected_offset;
        uint32_t expected_size = (uint32_t)(remaining > METADATA_CHUNK_SIZE ? METADATA_CHUNK_SIZE : remaining);
        int valid = 0;

        if (request_expect(endpoint->ip, endpoint->port, M_DOWNLOAD_REQ, request, request_size, M_DOWNLOAD_REP, &response) == 0)
        {
            if (transfer_decode_chunk(response.payload, response.header.payload_size, TRANSFER_DOWNLOAD_CHUNK, &chunk) == 0 && memcmp(chunk.id.bytes, lookup->document.id.bytes, OBJECT_ID_SIZE) == 0 && chunk.index == index && chunk.offset == expected_offset && chunk.raw_size == expected_size && compression_lz4_decompress(chunk.data, chunk.compressed_size, chunk.raw_size, &raw) == 0 && content_sha256(raw, chunk.raw_size, hash) == 0 && memcmp(hash, chunk.hash, OBJECT_ID_SIZE) == 0 && pwrite_all(work->output_fd, raw, chunk.raw_size, chunk.offset) == 0)
            {
                valid = 1;
                (void)pthread_mutex_lock(&work->mutex);
                work->compressed_total += chunk.compressed_size;
                (void)pthread_mutex_unlock(&work->mutex);
            }
            free(raw);
            message_free(&response);
        }
        if (valid)
        {
            free(request);
            return 0;
        }
    }
    free(request);
    errno = EREMOTEIO;
    return -1;
}

static void *download_worker(void *argument)
{
    DownloadWork *work = argument;

    for (;;)
    {
        uint64_t index;

        (void)pthread_mutex_lock(&work->mutex);
        if (work->failed || work->next_index >= work->lookup->document.chunk_count)
        {
            (void)pthread_mutex_unlock(&work->mutex);
            break;
        }
        index = work->next_index++;
        work->state = TRANSFER_STARTED;
        (void)pthread_mutex_unlock(&work->mutex);
        (void)pthread_mutex_lock(&work->mutex);
        work->state = TRANSFER_TRANSFERRING;
        (void)pthread_mutex_unlock(&work->mutex);
        if (download_one(work, index) < 0)
        {
            download_fail(work, errno);
            break;
        }
    }
    return NULL;
}

static int execute_download_workers(DownloadWork *work, size_t worker_count)
{
    pthread_t *threads = calloc(worker_count, sizeof(*threads));
    size_t started = 0U;
    size_t index;

    if (threads == NULL)
    {
        return -1;
    }
    for (index = 0U; index < worker_count; ++index)
    {
        int error = pthread_create(&threads[index], NULL, download_worker, work);

        if (error != 0)
        {
            download_fail(work, error);
            break;
        }
        ++started;
    }
    for (index = 0U; index < started; ++index)
    {
        (void)pthread_join(threads[index], NULL);
    }
    free(threads);
    if (work->failed)
    {
        errno = work->saved_errno;
        return -1;
    }
    return 0;
}

static int lookup_document(const char *host, uint16_t port, const char *selector, TransferLookupResult *result)
{
    uint8_t *payload = NULL;
    uint32_t payload_size = 0U;
    Message response;
    int status = -1;

    if (transfer_encode_lookup_request(selector, &payload, &payload_size) < 0 || request_expect(host, port, M_LOOKUP, payload, payload_size, M_DOWNLOAD_REP, &response) < 0)
    {
        free(payload);
        return -1;
    }
    if (transfer_decode_lookup_result(response.payload, response.header.payload_size, result) == 0)
    {
        status = 0;
    }
    message_free(&response);
    free(payload);
    return status;
}

int file_client_download(const char *selector, const char *destination, const char *superpeer_host, uint16_t superpeer_port)
{
    TransferLookupResult lookup;
    DownloadWork work;
    ObjectID downloaded_id;
    uint64_t downloaded_size;
    char *part_path = NULL;
    int output_fd = -1;
    int owns_part = 0;
    size_t destination_size;
    size_t worker_count;
    struct timespec started;
    struct timespec finished;
    double elapsed;
    int status = -1;

    memset(&lookup, 0, sizeof(lookup));
    memset(&work, 0, sizeof(work));
    if (lookup_document(superpeer_host, superpeer_port, selector, &lookup) < 0)
    {
        return -1;
    }
    if (destination == NULL)
    {
        if (strchr(lookup.document.name, '/') != NULL || strcmp(lookup.document.name, ".") == 0 || strcmp(lookup.document.name, "..") == 0)
        {
            errno = EBADMSG;
            goto cleanup;
        }
        destination = lookup.document.name;
    }
    destination_size = strlen(destination);
    if (destination_size == 0U || destination_size > SIZE_MAX - PART_PATH_EXTRA || access(destination, F_OK) == 0)
    {
        errno = access(destination, F_OK) == 0 ? EEXIST : EINVAL;
        goto cleanup;
    }
    part_path = malloc(destination_size + PART_PATH_EXTRA);
    if (part_path == NULL)
    {
        goto cleanup;
    }
    (void)snprintf(part_path, destination_size + PART_PATH_EXTRA, "%s.part", destination);
    output_fd = open(part_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output_fd < 0)
    {
        goto cleanup;
    }
    owns_part = 1;
    if (lookup.document.file_size > (uint64_t)INT64_MAX || ftruncate(output_fd, (off_t)lookup.document.file_size) < 0 || pthread_mutex_init(&work.mutex, NULL) != 0)
    {
        goto cleanup;
    }
    work.output_fd = output_fd;
    work.lookup = &lookup;
    work.state = TRANSFER_CREATED;
    worker_count = transfer_worker_count(lookup.document.chunk_count);
    work.state = TRANSFER_QUEUED;
    printf("Transfer workers: %zu\n", worker_count);
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    if (execute_download_workers(&work, worker_count) < 0)
    {
        (void)pthread_mutex_destroy(&work.mutex);
        goto cleanup;
    }
    work.state = TRANSFER_VERIFYING;
    if (fsync(output_fd) < 0 || object_id_file(part_path, &downloaded_id, &downloaded_size) < 0 || downloaded_size != lookup.document.file_size || memcmp(downloaded_id.bytes, lookup.document.id.bytes, OBJECT_ID_SIZE) != 0 || link(part_path, destination) < 0 || unlink(part_path) < 0)
    {
        (void)pthread_mutex_destroy(&work.mutex);
        goto cleanup;
    }
    (void)pthread_mutex_destroy(&work.mutex);
    work.state = TRANSFER_FINISHED;
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    elapsed = (double)(finished.tv_sec - started.tv_sec) + (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    printf("Original bytes: %" PRIu64 "\nCompressed bytes: %" PRIu64 "\nDuration: %.3f s\nThroughput: %.2f MiB/s\nState: FINISHED\nDownload completed\nSHA-256 verified\n", lookup.document.file_size, work.compressed_total, elapsed, elapsed > 0.0 ? ((double)lookup.document.file_size / 1048576.0) / elapsed : 0.0);
    status = 0;

cleanup:
    if (output_fd >= 0)
    {
        (void)close(output_fd);
    }
    if (status < 0 && owns_part)
    {
        (void)unlink(part_path);
    }
    free(part_path);
    transfer_lookup_result_free(&lookup);
    return status;
}

int file_client_benchmark_lz4(const char *path)
{
    uint8_t *raw = NULL;
    int fd = -1;
    struct stat information;
    struct timespec started;
    struct timespec finished;
    uint64_t offset = 0U;
    uint64_t compressed_total = 0U;
    double elapsed;
    int status = -1;

    if (content_validate_pdf(path) < 0 || stat(path, &information) < 0 || information.st_size <= 0)
    {
        return -1;
    }
    raw = malloc((size_t)METADATA_CHUNK_SIZE);
    fd = open(path, O_RDONLY);
    if (raw == NULL || fd < 0)
    {
        goto cleanup;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    while (offset < (uint64_t)information.st_size)
    {
        uint64_t remaining = (uint64_t)information.st_size - offset;
        size_t size = remaining > METADATA_CHUNK_SIZE ? (size_t)METADATA_CHUNK_SIZE : (size_t)remaining;
        uint8_t *compressed = NULL;
        size_t compressed_size = 0U;

        if (pread_all(fd, raw, size, offset) < 0 || compression_lz4_compress(raw, size, &compressed, &compressed_size) < 0)
        {
            free(compressed);
            goto cleanup;
        }
        compressed_total += compressed_size;
        offset += size;
        free(compressed);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
    elapsed = (double)(finished.tv_sec - started.tv_sec) + (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    printf("LZ4 benchmark (informativo)\nOriginal bytes: %" PRIu64 "\nCompressed bytes: %" PRIu64 "\nDuration: %.6f s\nCompression throughput: %.2f MiB/s\n", offset, compressed_total, elapsed, elapsed > 0.0 ? ((double)offset / 1048576.0) / elapsed : 0.0);
    status = 0;

cleanup:
    if (fd >= 0)
    {
        (void)close(fd);
    }
    free(raw);
    return status;
}
