#ifndef REMOTE_ERROR_H
#define REMOTE_ERROR_H
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
/* Códigos wire independentes dos valores de errno do sistema operacional. */
static inline void remote_error_encode(int error, uint8_t output[2])
{
    output[0] = 1U;
    switch (error)
    {
    case ENOENT: output[1] = 1U; break;
    case ENOTUNIQ: output[1] = 2U; break;
    case EEXIST: output[1] = 3U; break;
    case EBADMSG: case EINVAL: output[1] = 4U; break;
    case ENOTSUP: output[1] = 5U; break;
    case EACCES: output[1] = 6U; break;
    case ENODATA: output[1] = 7U; break;
    case EMSGSIZE: case EOVERFLOW: output[1] = 8U; break;
    default: output[1] = 9U; break;
    }
}
static inline int remote_error_decode(const uint8_t *data, size_t size)
{
    if (data == NULL || size != 2U || data[0] != 1U) return EREMOTEIO;
    switch (data[1])
    {
    case 1U: return ENOENT;
    case 2U: return ENOTUNIQ;
    case 3U: return EEXIST;
    case 4U: return EBADMSG;
    case 5U: return ENOTSUP;
    case 6U: return EACCES;
    case 7U: return ENODATA;
    case 8U: return EMSGSIZE;
    default: return EIO;
    }
}
#endif
