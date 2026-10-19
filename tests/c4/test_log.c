#define _POSIX_C_SOURCE 200809L
#include "c4_log.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    /* Verifica persistência, ordem, idempotência, conflito, checksum e serialização do log C4. */
    char path[] = "/tmp/pd-c4-log-XXXXXX";
    const uint8_t transaction[16] = {1U, 2U, 3U};
    const uint8_t transaction_two[16] = {4U, 5U, 6U};
    const uint8_t payload[] = {9U, 8U, 7U, 6U};
    uint8_t *encoded = NULL;
    uint32_t encoded_size = 0U;
    C4Log *log = NULL;
    C4LogEntry entry;
    int descriptor = mkstemp(path);

    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(c4_log_open(path, &log) == 0);
    assert(c4_log_append(log, transaction, 1U, payload, sizeof(payload)) == 0);
    assert(c4_log_last_index(log) == 1U);
    assert(c4_log_last_version(log) == 1U);
    assert(c4_log_append(log, transaction_two, 2U, payload, sizeof(payload)) == 0);
    assert(c4_log_last_index(log) == 2U);
    assert(c4_log_append(log, transaction, 1U, payload, sizeof(payload)) == 0);
    assert(c4_log_get_transaction(log, transaction, &entry) == 0 && entry.index == 1U);
    c4_log_entry_free(&entry);
    errno = 0;
    assert(c4_log_append(log, transaction, 2U, payload, sizeof(payload)) < 0 && errno == EEXIST);
    c4_log_close(log);

    assert(c4_log_open(path, &log) == 0);
    assert(c4_log_last_index(log) == 2U);
    assert(c4_log_last_version(log) == 2U);
    assert(c4_log_get(log, 1U, &entry) == 0);
    assert(entry.transaction_id[0] == 1U && entry.version == 1U && entry.payload_size == sizeof(payload));
    assert(memcmp(entry.payload, payload, sizeof(payload)) == 0);
    assert(c4_log_entry_encode(&entry, &encoded, &encoded_size) == 0);
    C4LogEntry decoded;
    assert(c4_log_entry_decode(encoded, encoded_size, &decoded) == 0);
    assert(decoded.index == entry.index && decoded.version == entry.version && decoded.checksum == entry.checksum);
    assert(memcmp(decoded.payload, entry.payload, entry.payload_size) == 0);
    c4_log_entry_free(&decoded);
    encoded[encoded_size - 1U] ^= 1U;
    errno = 0;
    assert(c4_log_entry_decode(encoded, encoded_size, &decoded) < 0 && errno == EBADMSG);
    free(encoded);
    c4_log_entry_free(&entry);
    c4_log_close(log);
    assert(unlink(path) == 0);
    puts("C4 log local: persistencia, ordem, replay idempotente, conflito e wire OK");
    return 0;
}
