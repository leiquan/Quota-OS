#include "quotaos.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *p = dst;
    while (n--)
        *p++ = (uint8_t)c;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* 复制到定长缓冲区，总是以 0 结尾。 */
void str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; i < cap - 1 && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}
