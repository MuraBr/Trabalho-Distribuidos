#ifndef CONTENT_H
#define CONTENT_H

#include "metadata.h"

#include <stddef.h>
#include <stdint.h>

int content_sha256(const uint8_t *data, size_t size, uint8_t digest[OBJECT_ID_SIZE]);
int content_validate_pdf(const char *path);
int content_pdf_name(const char *path);
int content_sync_parent(const char *path);
int content_basename(const char *path, char output[METADATA_NAME_SIZE]);

#endif
