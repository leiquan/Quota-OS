/* The stock AArch64 runtime expects an fd for PartitionAlloc entropy.
 * In an empty root, use the guest's anonymous read-only RNG capability. */
#include <stdarg.h>
extern long syscall(long, ...);
extern int *__errno_location(void);

int open64(const char *path, int flags, ...)
{
    unsigned mode = 0;
    if ((flags & 64) || (flags & 020000000)) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, unsigned);
        va_end(args);
    }
    long fd = syscall(56L, -100L, path, (long)flags, (long)mode);
    if (fd >= 0 || *__errno_location() != 2 || (flags & ~0x80000)) return (int)fd;
    static const char device[] = "/dev/urandom";
    for (unsigned i = 0; i < sizeof device; ++i)
        if (path[i] != device[i]) return -1;
    return (int)syscall(0x5152L, (long)flags);
}
