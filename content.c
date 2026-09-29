#define _POSIX_C_SOURCE 200809L
#include "content.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <openssl/sha.h>

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
    FILE *file;

    if (path == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (!content_pdf_name(path)) { errno = EINVAL; return -1; }
    file = fopen(path, "rb");
    if (file == NULL)
    {
        return -1;
    }
    if (fclose(file) != 0)
    {
        return -1;
    }
    /* C2 valida o tipo pela extensão; fixtures sintéticas também são aceitas. */
    return 0;
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

int content_pdf_name(const char *path)
{
    if (path == NULL) return 0;
    size_t length = strlen(path);
    return length >= 4U && path[length - 4U] == '.' && tolower((unsigned char)path[length - 3U]) == 'p' && tolower((unsigned char)path[length - 2U]) == 'd' && tolower((unsigned char)path[length - 1U]) == 'f';
}

int content_sync_parent(const char *path)
{
    char parent[4096];
    if (path == NULL || strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash == NULL) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int status = fsync(fd);
    int error = errno;
    close(fd);
    errno = error;
    return status;
}
