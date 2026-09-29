#include "compression.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* ABI estável da liblz4; o ambiente possui a biblioteca de execução sem os headers de desenvolvimento. */
extern int LZ4_compressBound(int inputSize);
extern int LZ4_compress_default(const char *src, char *dst, int srcSize, int dstCapacity);
extern int LZ4_decompress_safe(const char *src, char *dst, int compressedSize, int dstCapacity);

int compression_lz4_bound(size_t input_size, size_t *bound)
{
    int result;

    if (bound == NULL || input_size > (size_t)INT_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    result = LZ4_compressBound((int)input_size);
    if (result <= 0)
    {
        errno = EOVERFLOW;
        return -1;
    }
    *bound = (size_t)result;
    return 0;
}

int compression_lz4_compress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size)
{
    uint8_t *compressed;
    size_t capacity;
    int result;

    if (input == NULL || output == NULL || output_size == NULL || input_size == 0U)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    *output_size = 0U;
    if (compression_lz4_bound(input_size, &capacity) < 0)
    {
        return -1;
    }
    compressed = malloc(capacity);
    if (compressed == NULL)
    {
        return -1;
    }
    result = LZ4_compress_default((const char *)input, (char *)compressed, (int)input_size, (int)capacity);
    if (result <= 0)
    {
        free(compressed);
        errno = EIO;
        return -1;
    }
    *output = compressed;
    *output_size = (size_t)result;
    return 0;
}

int compression_lz4_decompress(const uint8_t *input, size_t input_size, size_t expected_size, uint8_t **output)
{
    uint8_t *decompressed;
    int result;

    if (input == NULL || output == NULL || input_size == 0U || input_size > (size_t)INT_MAX || expected_size == 0U || expected_size > (size_t)INT_MAX)
    {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    decompressed = malloc(expected_size);
    if (decompressed == NULL)
    {
        return -1;
    }
    result = LZ4_decompress_safe((const char *)input, (char *)decompressed, (int)input_size, (int)expected_size);
    if (result < 0 || (size_t)result != expected_size)
    {
        free(decompressed);
        errno = EBADMSG;
        return -1;
    }
    *output = decompressed;
    return 0;
}
