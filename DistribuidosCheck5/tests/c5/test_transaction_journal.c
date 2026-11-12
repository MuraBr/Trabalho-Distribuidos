#define _POSIX_C_SOURCE 200809L
#include "c4_log.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void remove_files(const char *path)
{
    char journal[256];
    (void)unlink(path);
    (void)snprintf(journal, sizeof(journal), "%s.txn", path);
    (void)unlink(journal);
}

int main(void)
{
    char path[] = "/tmp/c5-journal-XXXXXX";
    uint8_t payload[] = {1U, 2U, 3U};
    C4LogEntry entry = {.index = 1U, .version = 1U, .payload = payload, .payload_size = sizeof(payload)};
    C4LogEntry result;
    C4TransactionState state;
    C4Log *log = NULL;
    C4LogEntry conflict;
    int fd = mkstemp(path);
    assert(fd >= 0);
    (void)close(fd);
    remove_files(path);
    memset(entry.transaction_id, 0x11, sizeof(entry.transaction_id));
    entry.checksum = c4_log_entry_checksum(&entry);
    assert(c4_log_open(path, &log) == 0);
    assert(c4_log_transaction_state(log, entry.transaction_id, &state) == 0 && state == C4_TXN_UNKNOWN);
    assert(c4_log_prepare(log, &entry) == 0);
    assert(c4_log_prepare(log, &entry) == 0);
    assert(c4_log_transaction_state(log, entry.transaction_id, &state) == 0 && state == C4_TXN_PREPARED);
    assert(c4_log_get_transaction(log, entry.transaction_id, &result) < 0 && errno == ENOENT);
    assert(c4_log_get_prepared(log, entry.transaction_id, &result) == 0);
    c4_log_entry_free(&result);
    conflict = entry;
    conflict.payload = (uint8_t *)"bad";
    conflict.checksum = c4_log_entry_checksum(&conflict);
    assert(c4_log_prepare(log, &conflict) < 0 && errno == EEXIST);
    assert(c4_log_commit(log, entry.transaction_id, &result) == 0);
    c4_log_entry_free(&result);
    assert(c4_log_commit(log, entry.transaction_id, &result) == 0);
    c4_log_entry_free(&result);
    c4_log_close(log);
    log = NULL;
    (void)unlink(path);
    assert(c4_log_open(path, &log) == 0);
    assert(c4_log_transaction_state(log, entry.transaction_id, &state) == 0 && state == C4_TXN_COMMITTED);
    assert(c4_log_get_transaction(log, entry.transaction_id, &result) == 0 && result.index == 1U);
    c4_log_entry_free(&result);
    entry.index = 2U;
    entry.version = 2U;
    memset(entry.transaction_id, 0x22, sizeof(entry.transaction_id));
    entry.checksum = c4_log_entry_checksum(&entry);
    assert(c4_log_prepare(log, &entry) == 0);
    assert(c4_log_abort(log, entry.transaction_id) == 0);
    assert(c4_log_abort(log, entry.transaction_id) == 0);
    assert(c4_log_commit(log, entry.transaction_id, NULL) < 0 && errno == ENOENT);
    assert(c4_log_transaction_state(log, entry.transaction_id, &state) == 0 && state == C4_TXN_ABORTED);
    memset(entry.transaction_id, 0x33, sizeof(entry.transaction_id));
    assert(c4_log_abort(log, entry.transaction_id) == 0);
    c4_log_close(log);
    log = NULL;
    assert(c4_log_open(path, &log) == 0);
    assert(c4_log_transaction_state(log, entry.transaction_id, &state) == 0 && state == C4_TXN_ABORTED);
    c4_log_close(log);
    remove_files(path);
    {
        char corrupt_path[] = "/tmp/c5-journal-corrupt-XXXXXX";
        char corrupt_journal[128];
        uint8_t invalid[16] = {0U};
        fd = mkstemp(corrupt_path);
        assert(fd >= 0);
        (void)close(fd);
        (void)snprintf(corrupt_journal, sizeof(corrupt_journal), "%s.txn", corrupt_path);
        fd = open(corrupt_journal, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0);
        assert(write(fd, invalid, sizeof(invalid)) == (ssize_t)sizeof(invalid));
        (void)close(fd);
        assert(c4_log_open(corrupt_path, &log) < 0 && errno == EBADMSG);
        remove_files(corrupt_path);
    }
    puts("transaction journal lifecycle/replay OK");
    return 0;
}
