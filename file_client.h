#ifndef FILE_CLIENT_H
#define FILE_CLIENT_H

#include <stdint.h>
#include <stdio.h>
#include "node.h"

typedef struct
{
    NodeID source;
    FILE *output;
    const char *directory;
} FileSession;

int file_client_upload(const FileSession *session, const char *path, const char *peer_host, uint16_t peer_port);
int file_client_download(const FileSession *session, const char *selector, const char *destination, const char *superpeer_host, uint16_t superpeer_port);
int file_client_benchmark_lz4(const char *path);

#endif
