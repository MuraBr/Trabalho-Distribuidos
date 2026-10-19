#ifndef C4_LOG_H
#define C4_LOG_H

#include <stddef.h>
#include <stdint.h>

#define C4_LOG_HEADER_SIZE 44U

typedef struct C4Log C4Log;

typedef struct
{
    uint64_t index;
    uint64_t version;
    uint8_t transaction_id[16];
    uint8_t *payload;
    uint32_t payload_size;
    uint32_t checksum;
} C4LogEntry;

int c4_log_open(const char *path, C4Log **output);
void c4_log_close(C4Log *log);
int c4_log_append(C4Log *log, const uint8_t transaction_id[16], uint64_t version, const uint8_t *payload, uint32_t payload_size);
int c4_log_append_entry(C4Log *log, const C4LogEntry *entry);
int c4_log_entry_encode(const C4LogEntry *entry, uint8_t **output, uint32_t *output_size);
int c4_log_entry_decode(const uint8_t *bytes, uint32_t size, C4LogEntry *output);
int c4_log_get(C4Log *log, uint64_t index, C4LogEntry *output);
int c4_log_get_transaction(C4Log *log, const uint8_t transaction_id[16], C4LogEntry *output);
uint64_t c4_log_last_index(C4Log *log);
uint64_t c4_log_last_version(C4Log *log);
uint32_t c4_log_entry_checksum(const C4LogEntry *entry);
void c4_log_entry_free(C4LogEntry *entry);

#endif
