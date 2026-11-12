#define _POSIX_C_SOURCE 200809L
#include "c4_log.h"

#include "network.h"
#include "protocol.h"
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define C4_LOG_MAGIC "C4LG"
#define C4_TXN_MAGIC "C5TJ"
#define C4_TXN_HEADER_SIZE 16U
#define C4_TXN_PREPARE 1U
#define C4_TXN_COMMIT 2U
#define C4_TXN_ABORT 3U

typedef struct
{
    C4LogEntry entry;
} C4LogSlot;

typedef struct
{
    C4LogEntry entry;
    C4TransactionState state;
} C4Transaction;

struct C4Log
{
    int fd;
    pthread_mutex_t mutex;
    C4LogSlot *entries;
    size_t count;
    size_t capacity;
    int journal_fd;
    C4Transaction *transactions;
    size_t transaction_count;
    size_t transaction_capacity;
};

static int read_exact_at(int fd, uint8_t *buffer, size_t size, off_t offset, size_t *received)
{
    size_t total = 0U;
    while (total < size)
    {
        ssize_t amount = pread(fd, buffer + total, size - total, offset + (off_t)total);
        if (amount == 0)
        {
            break;
        }
        if (amount < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        total += (size_t)amount;
    }
    *received = total;
    return 0;
}

static int sync_parent_directory(const char *path)
{
    char *directory = strdup(path);
    char *separator;
    int fd;
    int result;
    int error;
    if (directory == NULL) return -1;
    separator = strrchr(directory, '/');
    if (separator == NULL) strcpy(directory, ".");
    else if (separator == directory) separator[1] = '\0';
    else *separator = '\0';
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(directory);
    if (fd < 0) return -1;
    result = fsync(fd);
    error = errno;
    (void)close(fd);
    errno = error;
    return result;
}

static int write_all(int fd, const uint8_t *buffer, size_t size)
{
    size_t total = 0U;
    while (total < size)
    {
        ssize_t amount = write(fd, buffer + total, size - total);
        if (amount < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (amount == 0)
        {
            errno = EIO;
            return -1;
        }
        total += (size_t)amount;
    }
    return 0;
}

void c4_log_entry_free(C4LogEntry *entry)
{
    if (entry != NULL)
    {
        free(entry->payload);
        memset(entry, 0, sizeof(*entry));
    }
}

uint32_t c4_log_entry_checksum(const C4LogEntry *entry)
{
    uint8_t header[C4_LOG_HEADER_SIZE] = {0U};
    uint8_t *bytes;
    uint32_t checksum;
    if (entry == NULL || (entry->payload_size > 0U && entry->payload == NULL) || entry->payload_size > MAX_PAYLOAD_SIZE)
    {
        errno = EINVAL;
        return 0U;
    }
    bytes = malloc(C4_LOG_HEADER_SIZE + (size_t)entry->payload_size);
    if (bytes == NULL)
    {
        return 0U;
    }
    memcpy(header, C4_LOG_MAGIC, 4U);
    wire_put_u64(header + 4U, entry->index);
    wire_put_u64(header + 12U, entry->version);
    memcpy(header + 20U, entry->transaction_id, sizeof(entry->transaction_id));
    wire_put_u32(header + 36U, entry->payload_size);
    wire_put_u32(header + 40U, 0U);
    memcpy(bytes, header, sizeof(header));
    if (entry->payload_size > 0U)
    {
        memcpy(bytes + sizeof(header), entry->payload, entry->payload_size);
    }
    checksum = protocol_calculate_crc32(bytes, sizeof(header) + (size_t)entry->payload_size);
    free(bytes);
    return checksum;
}

static int reserve_entry(C4Log *log)
{
    if (log->count == log->capacity)
    {
        size_t capacity = log->capacity == 0U ? 16U : log->capacity * 2U;
        C4LogSlot *entries;
        if (capacity < log->capacity || capacity > SIZE_MAX / sizeof(*entries))
        {
            errno = EOVERFLOW;
            return -1;
        }
        entries = realloc(log->entries, capacity * sizeof(*entries));
        if (entries == NULL)
        {
            return -1;
        }
        log->entries = entries;
        log->capacity = capacity;
    }
    return 0;
}

static int append_memory(C4Log *log, const C4LogEntry *entry)
{
    C4LogEntry *copy;
    if (reserve_entry(log) < 0)
    {
        return -1;
    }
    copy = &log->entries[log->count].entry;
    *copy = *entry;
    copy->payload = NULL;
    if (entry->payload_size > 0U)
    {
        copy->payload = malloc(entry->payload_size);
        if (copy->payload == NULL)
        {
            return -1;
        }
        memcpy(copy->payload, entry->payload, entry->payload_size);
    }
    ++log->count;
    return 0;
}

static int reserve_transaction(C4Log *log)
{
    if (log->transaction_count == log->transaction_capacity)
    {
        size_t capacity = log->transaction_capacity == 0U ? 16U : log->transaction_capacity * 2U;
        C4Transaction *transactions;
        if (capacity < log->transaction_capacity || capacity > SIZE_MAX / sizeof(*transactions))
        {
            errno = EOVERFLOW;
            return -1;
        }
        transactions = realloc(log->transactions, capacity * sizeof(*transactions));
        if (transactions == NULL) return -1;
        log->transactions = transactions;
        log->transaction_capacity = capacity;
    }
    return 0;
}

static C4Transaction *find_transaction(C4Log *log, const uint8_t transaction_id[16])
{
    size_t index;
    for (index = 0U; index < log->transaction_count; ++index)
    {
        if (memcmp(log->transactions[index].entry.transaction_id, transaction_id, 16U) == 0) return &log->transactions[index];
    }
    return NULL;
}

static int journal_record(C4Log *log, uint8_t type, const uint8_t *payload, uint32_t payload_size)
{
    uint8_t header[C4_TXN_HEADER_SIZE] = {0U};
    uint8_t *checked;
    off_t original_size;
    uint32_t checksum;
    if (payload_size > MAX_PAYLOAD_SIZE)
    {
        errno = EMSGSIZE;
        return -1;
    }
    checked = malloc(5U + (size_t)payload_size);
    if (checked == NULL) return -1;
    checked[0] = type;
    wire_put_u32(checked + 1U, payload_size);
    if (payload_size > 0U) memcpy(checked + 5U, payload, payload_size);
    checksum = protocol_calculate_crc32(checked, 5U + (size_t)payload_size);
    free(checked);
    memcpy(header, C4_TXN_MAGIC, 4U);
    header[4] = type;
    wire_put_u32(header + 8U, payload_size);
    wire_put_u32(header + 12U, checksum);
    original_size = lseek(log->journal_fd, 0, SEEK_END);
    if (original_size < 0 || write_all(log->journal_fd, header, sizeof(header)) < 0 || (payload_size > 0U && write_all(log->journal_fd, payload, payload_size) < 0) || fsync(log->journal_fd) < 0)
    {
        int error = errno;
        if (original_size >= 0) (void)ftruncate(log->journal_fd, original_size);
        errno = error;
        return -1;
    }
    return 0;
}

static int copy_entry(C4LogEntry *output, const C4LogEntry *source)
{
    *output = *source;
    output->payload = NULL;
    if (source->payload_size > 0U)
    {
        output->payload = malloc(source->payload_size);
        if (output->payload == NULL) return -1;
        memcpy(output->payload, source->payload, source->payload_size);
    }
    return 0;
}

static int append_locked(C4Log *log, const C4LogEntry *entry, int persist);
static int same_entry(const C4LogEntry *left, const C4LogEntry *right);

static int load_journal(C4Log *log)
{
    struct stat status;
    off_t offset = 0;
    if (fstat(log->journal_fd, &status) < 0) return -1;
    while (offset < status.st_size)
    {
        uint8_t header[C4_TXN_HEADER_SIZE];
        uint8_t *payload = NULL;
        uint8_t *checked = NULL;
        size_t received = 0U;
        uint32_t payload_size;
        uint8_t type;
        uint32_t expected_crc;
        uint32_t actual_crc;
        C4Transaction *transaction;
        if (read_exact_at(log->journal_fd, header, sizeof(header), offset, &received) < 0 || received != sizeof(header) || memcmp(header, C4_TXN_MAGIC, 4U) != 0)
        {
            errno = EBADMSG;
            return -1;
        }
        type = header[4];
        payload_size = wire_get_u32(header + 8U);
        expected_crc = wire_get_u32(header + 12U);
        if (payload_size > MAX_PAYLOAD_SIZE || offset + (off_t)sizeof(header) + (off_t)payload_size > status.st_size)
        {
            errno = EBADMSG;
            return -1;
        }
        if (payload_size > 0U)
        {
            payload = malloc(payload_size);
            if (payload == NULL) return -1;
            if (read_exact_at(log->journal_fd, payload, payload_size, offset + (off_t)sizeof(header), &received) < 0 || received != payload_size)
            {
                free(payload);
                errno = EBADMSG;
                return -1;
            }
        }
        checked = malloc(5U + (size_t)payload_size);
        if (checked == NULL)
        {
            free(payload);
            return -1;
        }
        checked[0] = type;
        wire_put_u32(checked + 1U, payload_size);
        if (payload_size > 0U) memcpy(checked + 5U, payload, payload_size);
        actual_crc = protocol_calculate_crc32(checked, 5U + (size_t)payload_size);
        free(checked);
        if (actual_crc != expected_crc)
        {
            free(payload);
            errno = EBADMSG;
            return -1;
        }
        if (type == C4_TXN_PREPARE)
        {
            C4LogEntry entry;
            C4LogEntry committed_entry;
            uint64_t expected_index = (uint64_t)log->count + 1U;
            uint64_t expected_version = log->count == 0U ? 1U : log->entries[log->count - 1U].entry.version + 1U;
            size_t transaction_index;
            int already_committed = 0;
            memset(&entry, 0, sizeof(entry));
            memset(&committed_entry, 0, sizeof(committed_entry));
            if (c4_log_entry_decode(payload, payload_size, &entry) < 0 || find_transaction(log, entry.transaction_id) != NULL || reserve_transaction(log) < 0)
            {
                c4_log_entry_free(&entry);
                free(payload);
                errno = EBADMSG;
                return -1;
            }
            if (c4_log_get_transaction(log, entry.transaction_id, &committed_entry) == 0)
            {
                already_committed = same_entry(&committed_entry, &entry);
                c4_log_entry_free(&committed_entry);
                if (!already_committed)
                {
                    c4_log_entry_free(&entry);
                    free(payload);
                    errno = EBADMSG;
                    return -1;
                }
            }
            else if (errno != ENOENT)
            {
                c4_log_entry_free(&entry);
                free(payload);
                return -1;
            }
            for (transaction_index = 0U; transaction_index < log->transaction_count; ++transaction_index)
            {
                C4Transaction *pending = &log->transactions[transaction_index];
                if (pending->state == C4_TXN_PREPARED && pending->entry.index >= expected_index)
                {
                    expected_index = pending->entry.index + 1U;
                    expected_version = pending->entry.version + 1U;
                }
            }
            if (!already_committed && (entry.index != expected_index || entry.version != expected_version))
            {
                c4_log_entry_free(&entry);
                free(payload);
                errno = EBADMSG;
                return -1;
            }
            log->transactions[log->transaction_count].entry = entry;
            log->transactions[log->transaction_count].state = C4_TXN_PREPARED;
            ++log->transaction_count;
        }
        else if ((type == C4_TXN_COMMIT || type == C4_TXN_ABORT) && payload_size == 16U)
        {
            transaction = find_transaction(log, payload);
            if (transaction == NULL && type == C4_TXN_ABORT && reserve_transaction(log) == 0)
            {
                transaction = &log->transactions[log->transaction_count++];
                memset(transaction, 0, sizeof(*transaction));
                memcpy(transaction->entry.transaction_id, payload, 16U);
                transaction->state = C4_TXN_ABORTED;
            }
            if (transaction == NULL || transaction->state != C4_TXN_PREPARED)
            {
                if (type == C4_TXN_ABORT && transaction != NULL && transaction->state == C4_TXN_ABORTED)
                {
                    free(payload);
                    offset += (off_t)sizeof(header) + (off_t)payload_size;
                    continue;
                }
                free(payload);
                errno = EBADMSG;
                return -1;
            }
            transaction->state = type == C4_TXN_COMMIT ? C4_TXN_COMMITTED : C4_TXN_ABORTED;
            if (type == C4_TXN_COMMIT && append_locked(log, &transaction->entry, 1) < 0)
            {
                free(payload);
                return -1;
            }
        }
        else
        {
            free(payload);
            errno = EBADMSG;
            return -1;
        }
        free(payload);
        offset += (off_t)sizeof(header) + (off_t)payload_size;
    }
    return 0;
}

static int same_entry(const C4LogEntry *left, const C4LogEntry *right)
{
    return left->index == right->index && left->version == right->version && left->checksum == right->checksum && left->payload_size == right->payload_size && memcmp(left->transaction_id, right->transaction_id, sizeof(left->transaction_id)) == 0 && (left->payload_size == 0U || memcmp(left->payload, right->payload, left->payload_size) == 0);
}

static int append_locked(C4Log *log, const C4LogEntry *entry, int persist)
{
    uint8_t header[C4_LOG_HEADER_SIZE] = {0U};
    off_t original_size;
    size_t index;
    if (entry == NULL || entry->index == 0U || entry->version == 0U || (entry->payload_size > 0U && entry->payload == NULL) || entry->payload_size > MAX_PAYLOAD_SIZE || entry->checksum != c4_log_entry_checksum(entry))
    {
        errno = EBADMSG;
        return -1;
    }
    for (index = 0U; index < log->count; ++index)
    {
        C4LogEntry *old = &log->entries[index].entry;
        if (memcmp(old->transaction_id, entry->transaction_id, sizeof(old->transaction_id)) == 0)
        {
            if (same_entry(old, entry))
            {
                return 0;
            }
            errno = EEXIST;
            return -1;
        }
    }
    if (entry->index != (uint64_t)log->count + 1U || entry->version != (log->count == 0U ? 1U : log->entries[log->count - 1U].entry.version + 1U))
    {
        errno = ERANGE;
        return -1;
    }
    if (persist)
    {
        memcpy(header, C4_LOG_MAGIC, 4U);
        wire_put_u64(header + 4U, entry->index);
        wire_put_u64(header + 12U, entry->version);
        memcpy(header + 20U, entry->transaction_id, sizeof(entry->transaction_id));
        wire_put_u32(header + 36U, entry->payload_size);
        wire_put_u32(header + 40U, entry->checksum);
        original_size = lseek(log->fd, 0, SEEK_END);
        if (original_size < 0 || write_all(log->fd, header, sizeof(header)) < 0 || (entry->payload_size > 0U && write_all(log->fd, entry->payload, entry->payload_size) < 0) || fsync(log->fd) < 0)
        {
            int error = errno;
            if (original_size >= 0) (void)ftruncate(log->fd, original_size);
            errno = error;
            return -1;
        }
    }
    return append_memory(log, entry);
}

int c4_log_open(const char *path, C4Log **output)
{
    C4Log *log;
    struct stat status;
    char *journal_path;
    off_t offset = 0;
    int mutex_status;
    if (path == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    log = calloc(1U, sizeof(*log));
    if (log == NULL)
    {
        return -1;
    }
    log->fd = -1;
    log->journal_fd = -1;
    log->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log->fd < 0)
    {
        int error = errno;
        free(log);
        errno = error;
        return -1;
    }
    mutex_status = pthread_mutex_init(&log->mutex, NULL);
    if (mutex_status != 0 || fstat(log->fd, &status) < 0)
    {
        int error = mutex_status != 0 ? mutex_status : errno;
        (void)close(log->fd);
        free(log);
        errno = error;
        return -1;
    }
    while (offset < status.st_size)
    {
        uint8_t header[C4_LOG_HEADER_SIZE];
        size_t received = 0U;
        C4LogEntry entry;
        memset(&entry, 0, sizeof(entry));
        if (read_exact_at(log->fd, header, sizeof(header), offset, &received) < 0)
        {
            goto fail;
        }
        if (received < sizeof(header))
        {
            if (ftruncate(log->fd, offset) < 0) goto fail;
            break;
        }
        if (memcmp(header, C4_LOG_MAGIC, 4U) != 0)
        {
            errno = EBADMSG;
            goto fail;
        }
        entry.index = wire_get_u64(header + 4U);
        entry.version = wire_get_u64(header + 12U);
        memcpy(entry.transaction_id, header + 20U, sizeof(entry.transaction_id));
        entry.payload_size = wire_get_u32(header + 36U);
        entry.checksum = wire_get_u32(header + 40U);
        if (entry.payload_size > MAX_PAYLOAD_SIZE)
        {
            errno = EBADMSG;
            goto fail;
        }
        if (entry.payload_size > 0U)
        {
            entry.payload = malloc(entry.payload_size);
            if (entry.payload == NULL) goto fail;
            if (read_exact_at(log->fd, entry.payload, entry.payload_size, offset + (off_t)sizeof(header), &received) < 0)
            {
                c4_log_entry_free(&entry);
                goto fail;
            }
            if (received < entry.payload_size)
            {
                c4_log_entry_free(&entry);
                if (ftruncate(log->fd, offset) < 0) goto fail;
                break;
            }
        }
        if (append_locked(log, &entry, 0) < 0)
        {
            c4_log_entry_free(&entry);
            goto fail;
        }
        offset += (off_t)sizeof(header) + (off_t)entry.payload_size;
        c4_log_entry_free(&entry);
    }
    journal_path = malloc(strlen(path) + sizeof(".txn"));
    if (journal_path == NULL) goto fail;
    (void)sprintf(journal_path, "%s.txn", path);
    log->journal_fd = open(journal_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log->journal_fd >= 0 && sync_parent_directory(journal_path) < 0)
    {
        int error = errno;
        (void)close(log->journal_fd);
        log->journal_fd = -1;
        free(journal_path);
        errno = error;
        goto fail;
    }
    free(journal_path);
    if (log->journal_fd < 0 || load_journal(log) < 0) goto fail;
    *output = log;
    return 0;

fail:
    {
        int error = errno;
        c4_log_close(log);
        errno = error;
        return -1;
    }
}

void c4_log_close(C4Log *log)
{
    size_t index;
    if (log == NULL)
    {
        return;
    }
    for (index = 0U; index < log->count; ++index)
    {
        c4_log_entry_free(&log->entries[index].entry);
    }
    free(log->entries);
    for (index = 0U; index < log->transaction_count; ++index) c4_log_entry_free(&log->transactions[index].entry);
    free(log->transactions);
    if (log->fd >= 0) (void)close(log->fd);
    if (log->journal_fd >= 0) (void)close(log->journal_fd);
    (void)pthread_mutex_destroy(&log->mutex);
    free(log);
}

int c4_log_append_entry(C4Log *log, const C4LogEntry *entry)
{
    int result;
    if (log == NULL || entry == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&log->mutex);
    result = append_locked(log, entry, 1);
    (void)pthread_mutex_unlock(&log->mutex);
    return result;
}

int c4_log_entry_encode(const C4LogEntry *entry, uint8_t **output, uint32_t *output_size)
{
    uint8_t *bytes;
    if (entry == NULL || output == NULL || output_size == NULL || entry->checksum != c4_log_entry_checksum(entry))
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    *output_size = 0U;
    if (entry->payload_size > MAX_PAYLOAD_SIZE - C4_LOG_HEADER_SIZE)
    {
        errno = EMSGSIZE;
        return -1;
    }
    bytes = malloc(C4_LOG_HEADER_SIZE + (size_t)entry->payload_size);
    if (bytes == NULL)
    {
        return -1;
    }
    memcpy(bytes, C4_LOG_MAGIC, 4U);
    wire_put_u64(bytes + 4U, entry->index);
    wire_put_u64(bytes + 12U, entry->version);
    memcpy(bytes + 20U, entry->transaction_id, sizeof(entry->transaction_id));
    wire_put_u32(bytes + 36U, entry->payload_size);
    wire_put_u32(bytes + 40U, entry->checksum);
    if (entry->payload_size > 0U) memcpy(bytes + C4_LOG_HEADER_SIZE, entry->payload, entry->payload_size);
    *output = bytes;
    *output_size = C4_LOG_HEADER_SIZE + entry->payload_size;
    return 0;
}

int c4_log_entry_decode(const uint8_t *bytes, uint32_t size, C4LogEntry *output)
{
    uint32_t payload_size;
    if (bytes == NULL || output == NULL || size < C4_LOG_HEADER_SIZE || memcmp(bytes, C4_LOG_MAGIC, 4U) != 0)
    {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, sizeof(*output));
    payload_size = wire_get_u32(bytes + 36U);
    if (payload_size > MAX_PAYLOAD_SIZE || size != C4_LOG_HEADER_SIZE + payload_size)
    {
        errno = EMSGSIZE;
        return -1;
    }
    output->index = wire_get_u64(bytes + 4U);
    output->version = wire_get_u64(bytes + 12U);
    memcpy(output->transaction_id, bytes + 20U, sizeof(output->transaction_id));
    output->payload_size = payload_size;
    output->checksum = wire_get_u32(bytes + 40U);
    if (payload_size > 0U)
    {
        output->payload = malloc(payload_size);
        if (output->payload == NULL) return -1;
        memcpy(output->payload, bytes + C4_LOG_HEADER_SIZE, payload_size);
    }
    if (output->index == 0U || output->version == 0U || output->checksum != c4_log_entry_checksum(output))
    {
        c4_log_entry_free(output);
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

int c4_log_append(C4Log *log, const uint8_t transaction_id[16], uint64_t version, const uint8_t *payload, uint32_t payload_size)
{
    C4LogEntry entry;
    int result;
    size_t index;
    if (log == NULL || transaction_id == NULL || (payload_size > 0U && payload == NULL) || payload_size > MAX_PAYLOAD_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    memset(&entry, 0, sizeof(entry));
    entry.index = 0U;
    entry.version = version;
    memcpy(entry.transaction_id, transaction_id, sizeof(entry.transaction_id));
    entry.payload = (uint8_t *)payload;
    entry.payload_size = payload_size;
    (void)pthread_mutex_lock(&log->mutex);
    entry.index = (uint64_t)log->count + 1U;
    for (index = 0U; index < log->count; ++index)
    {
        C4LogEntry *old = &log->entries[index].entry;
        if (memcmp(old->transaction_id, transaction_id, sizeof(old->transaction_id)) == 0)
        {
            result = old->version == version && old->payload_size == payload_size && (payload_size == 0U || memcmp(old->payload, payload, payload_size) == 0) ? 0 : -1;
            if (result < 0) errno = EEXIST;
            (void)pthread_mutex_unlock(&log->mutex);
            return result;
        }
    }
    entry.checksum = c4_log_entry_checksum(&entry);
    result = append_locked(log, &entry, 1);
    (void)pthread_mutex_unlock(&log->mutex);
    return result;
}

int c4_log_get(C4Log *log, uint64_t index, C4LogEntry *output)
{
    C4LogEntry *source;
    if (log == NULL || output == NULL || index == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, sizeof(*output));
    (void)pthread_mutex_lock(&log->mutex);
    if (index > log->count)
    {
        (void)pthread_mutex_unlock(&log->mutex);
        errno = ENOENT;
        return -1;
    }
    source = &log->entries[(size_t)(index - 1U)].entry;
    *output = *source;
    output->payload = NULL;
    if (source->payload_size > 0U)
    {
        output->payload = malloc(source->payload_size);
        if (output->payload == NULL)
        {
            (void)pthread_mutex_unlock(&log->mutex);
            return -1;
        }
        memcpy(output->payload, source->payload, source->payload_size);
    }
    (void)pthread_mutex_unlock(&log->mutex);
    return 0;
}

int c4_log_get_transaction(C4Log *log, const uint8_t transaction_id[16], C4LogEntry *output)
{
    size_t index;
    if (log == NULL || transaction_id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, sizeof(*output));
    (void)pthread_mutex_lock(&log->mutex);
    for (index = 0U; index < log->count; ++index)
    {
        C4LogEntry *source = &log->entries[index].entry;
        if (memcmp(source->transaction_id, transaction_id, sizeof(source->transaction_id)) != 0) continue;
        *output = *source;
        output->payload = NULL;
        if (source->payload_size > 0U)
        {
            output->payload = malloc(source->payload_size);
            if (output->payload == NULL)
            {
                (void)pthread_mutex_unlock(&log->mutex);
                return -1;
            }
            memcpy(output->payload, source->payload, source->payload_size);
        }
        (void)pthread_mutex_unlock(&log->mutex);
        return 0;
    }
    (void)pthread_mutex_unlock(&log->mutex);
    errno = ENOENT;
    return -1;
}

uint64_t c4_log_last_index(C4Log *log)
{
    uint64_t index;
    if (log == NULL) return 0U;
    (void)pthread_mutex_lock(&log->mutex);
    index = (uint64_t)log->count;
    (void)pthread_mutex_unlock(&log->mutex);
    return index;
}

uint64_t c4_log_last_version(C4Log *log)
{
    uint64_t version = 0U;
    if (log == NULL) return 0U;
    (void)pthread_mutex_lock(&log->mutex);
    if (log->count > 0U) version = log->entries[log->count - 1U].entry.version;
    (void)pthread_mutex_unlock(&log->mutex);
    return version;
}

int c4_log_prepare(C4Log *log, const C4LogEntry *entry)
{
    C4Transaction *existing;
    C4LogEntry decoded;
    uint8_t *encoded = NULL;
    uint32_t encoded_size = 0U;
    uint64_t expected_index;
    uint64_t expected_version;
    size_t index;
    int result = -1;
    if (log == NULL || entry == NULL || entry->index == 0U || entry->version == 0U || entry->checksum != c4_log_entry_checksum(entry))
    {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&log->mutex);
    existing = find_transaction(log, entry->transaction_id);
    if (existing != NULL)
    {
        result = same_entry(&existing->entry, entry) && existing->state != C4_TXN_ABORTED ? 0 : -1;
        if (result < 0) errno = EEXIST;
        (void)pthread_mutex_unlock(&log->mutex);
        return result;
    }
    expected_index = (uint64_t)log->count + 1U;
    expected_version = log->count == 0U ? 1U : log->entries[log->count - 1U].entry.version + 1U;
    for (index = 0U; index < log->transaction_count; ++index)
    {
        C4Transaction *transaction = &log->transactions[index];
        if (transaction->state == C4_TXN_PREPARED && transaction->entry.index >= expected_index)
        {
            expected_index = transaction->entry.index + 1U;
            expected_version = transaction->entry.version + 1U;
        }
    }
    if (entry->index != expected_index || entry->version != expected_version)
    {
        errno = ERANGE;
        goto done;
    }
    if (c4_log_entry_encode(entry, &encoded, &encoded_size) < 0 || reserve_transaction(log) < 0 || journal_record(log, C4_TXN_PREPARE, encoded, encoded_size) < 0) goto done;
    memset(&decoded, 0, sizeof(decoded));
    if (c4_log_entry_decode(encoded, encoded_size, &decoded) < 0) goto done;
    log->transactions[log->transaction_count].entry = decoded;
    log->transactions[log->transaction_count].state = C4_TXN_PREPARED;
    ++log->transaction_count;
    result = 0;
done:
    free(encoded);
    (void)pthread_mutex_unlock(&log->mutex);
    return result;
}

int c4_log_commit(C4Log *log, const uint8_t transaction_id[16], C4LogEntry *output)
{
    C4Transaction *transaction;
    int result = -1;
    if (log == NULL || transaction_id == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (output != NULL) memset(output, 0, sizeof(*output));
    (void)pthread_mutex_lock(&log->mutex);
    transaction = find_transaction(log, transaction_id);
    if (transaction == NULL || transaction->state == C4_TXN_ABORTED)
    {
        errno = ENOENT;
        goto done;
    }
    if (transaction->state == C4_TXN_PREPARED)
    {
        if (journal_record(log, C4_TXN_COMMIT, transaction_id, 16U) < 0) goto done;
        transaction->state = C4_TXN_COMMITTED;
    }
    if (append_locked(log, &transaction->entry, 1) < 0) goto done;
    if (output != NULL && copy_entry(output, &transaction->entry) < 0) goto done;
    result = 0;
done:
    (void)pthread_mutex_unlock(&log->mutex);
    return result;
}

int c4_log_abort(C4Log *log, const uint8_t transaction_id[16])
{
    C4Transaction *transaction;
    int result = -1;
    if (log == NULL || transaction_id == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&log->mutex);
    transaction = find_transaction(log, transaction_id);
    if (transaction != NULL && transaction->state == C4_TXN_ABORTED)
    {
        result = 0;
        goto done;
    }
    if (transaction != NULL && transaction->state != C4_TXN_PREPARED)
    {
        errno = EALREADY;
        goto done;
    }
    if (transaction == NULL)
    {
        if (reserve_transaction(log) < 0) goto done;
    }
    if (journal_record(log, C4_TXN_ABORT, transaction_id, 16U) < 0) goto done;
    if (transaction == NULL)
    {
        transaction = &log->transactions[log->transaction_count++];
        memset(transaction, 0, sizeof(*transaction));
        memcpy(transaction->entry.transaction_id, transaction_id, 16U);
    }
    transaction->state = C4_TXN_ABORTED;
    result = 0;
done:
    (void)pthread_mutex_unlock(&log->mutex);
    return result;
}

int c4_log_transaction_state(C4Log *log, const uint8_t transaction_id[16], C4TransactionState *output)
{
    C4Transaction *transaction;
    if (log == NULL || transaction_id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    (void)pthread_mutex_lock(&log->mutex);
    transaction = find_transaction(log, transaction_id);
    *output = transaction == NULL ? C4_TXN_UNKNOWN : transaction->state;
    (void)pthread_mutex_unlock(&log->mutex);
    return 0;
}

int c4_log_get_prepared(C4Log *log, const uint8_t transaction_id[16], C4LogEntry *output)
{
    C4Transaction *transaction;
    int result;
    if (log == NULL || transaction_id == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    memset(output, 0, sizeof(*output));
    (void)pthread_mutex_lock(&log->mutex);
    transaction = find_transaction(log, transaction_id);
    if (transaction == NULL || transaction->state != C4_TXN_PREPARED)
    {
        (void)pthread_mutex_unlock(&log->mutex);
        errno = ENOENT;
        return -1;
    }
    result = copy_entry(output, &transaction->entry);
    (void)pthread_mutex_unlock(&log->mutex);
    return result;
}
