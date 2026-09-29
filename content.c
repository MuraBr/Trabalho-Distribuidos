#include "content.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

extern unsigned char *SHA256(const unsigned char *data, size_t size, unsigned char *digest);

int content_sha256(const uint8_t *data, size_t size, uint8_t digest[OBJECT_ID_SIZE])
{
    if (digest == NULL || (data == NULL && size != 0U))
    {
        errno = EINVAL;
        return -1;
    }
    if (SHA256(data, size, digest) == NULL)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

int content_validate_pdf(const char *path)
{
    uint8_t prefix[1024];
    size_t size;
    size_t index;
    size_t length;
    FILE *file;

    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    length = strlen(path);
    if (length < 4U || path[length - 4U] != '.' || tolower((unsigned char)path[length - 3U]) != 'p' || tolower((unsigned char)path[length - 2U]) != 'd' || tolower((unsigned char)path[length - 1U]) != 'f')
    {
        errno = EINVAL;
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL)
    {
        return -1;
    }
    size = fread(prefix, 1U, sizeof(prefix), file);
    if (ferror(file))
    {
        int saved_errno = errno == 0 ? EIO : errno;
        fclose(file);
        errno = saved_errno;
        return -1;
    }
    if (fclose(file) != 0)
    {
        return -1;
    }
    for (index = 0U; index + 5U <= size; ++index)
    {
        if (memcmp(prefix + index, "%PDF-", 5U) == 0)
        {
            return 0;
        }
    }
    errno = EBADMSG;
    return -1;
}

int content_basename(const char *path, char output[METADATA_NAME_SIZE])
{
    const char *name;
    const char *slash;
    size_t length;

    if (path == NULL || output == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    slash = strrchr(path, '/');
    name = slash == NULL ? path : slash + 1;
    length = strlen(name);
    if (length == 0U || length >= METADATA_NAME_SIZE)
    {
        errno = EINVAL;
        return -1;
    }
    memcpy(output, name, length + 1U);
    return 0;
}
