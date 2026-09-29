#ifndef COMPRESSION_H
#define COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

#define COMPRESSION_LZ4 1U

/* Camada LZ4: os buffers retornados pertencem ao chamador e devem ser liberados com free(). */
int compression_lz4_bound(size_t input_size, size_t *bound);
int compression_lz4_compress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size);
int compression_lz4_decompress(const uint8_t *input, size_t input_size, size_t expected_size, uint8_t **output);

#endif
