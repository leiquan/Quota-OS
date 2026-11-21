/* Freestanding LP64 printf format macros used by this LVGL configuration.
 * Clang supplies stdint.h; no hosted libc or C++ library is linked. */
#ifndef QUOTA_INTTYPES_H
#define QUOTA_INTTYPES_H
#include <stdint.h>
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRIX64 "lX"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#endif
