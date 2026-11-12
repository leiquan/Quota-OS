#include "quotaos.h"
#include <stdarg.h>

static void put_s(const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        uart_putc(*s++);
}

static void put_u(uint64_t v, int base, int width, char pad)
{
    char tmp[32];
    int i = 0;
    if (base < 2)
        base = 10;
    if (v == 0)
        tmp[i++] = '0';
    while (v) {
        uint64_t d = v % (uint64_t)base;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= (uint64_t)base;
    }
    while (i < width)
        tmp[i++] = pad;
    while (i--)
        uart_putc(tmp[i]);
}

void printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            uart_putc(*fmt++);
            continue;
        }
        fmt++;
        char pad = ' ';
        int left = 0;
        int width = 0;
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int lng = 0;
        if (*fmt == 'l') {
            lng = 1;
            fmt++;
            if (*fmt == 'l')
                fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            if (!left)
                while (width > len) {
                    uart_putc(pad);
                    width--;
                }
            put_s(s);
            if (left)
                while (width > len) {
                    uart_putc(' ');
                    width--;
                }
            break;
        }
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                uart_putc('-');
                put_u((uint64_t)(-v), 10, width, pad);
            } else {
                put_u((uint64_t)v, 10, width, pad);
            }
            break;
        }
        case 'u': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_u(v, 10, width, pad);
            break;
        }
        case 'x': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            put_u(v, 16, width, pad);
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case '%':
            uart_putc('%');
            break;
        case 0:
            va_end(ap);
            return;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
        fmt++;
    }
    va_end(ap);
}
