#ifndef PSTD_STRING_H
#define PSTD_STRING_H

#include "pstd_int.h"

typedef __builtin_va_list va_list;
#define vaStart(v, last)  __builtin_va_start(v, last)
#define vaArg(v, t)       __builtin_va_arg(v, t)
#define vaEnd(v)          __builtin_va_end(v)

void *memcpy(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, usize n)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *x = a;
    const u8 *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }
    return 0;
}

usize strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    while (n--) {
        if (*a != *b)
            return (u8)*a - (u8)*b;
        if (!*a)
            return 0;
        a++;
        b++;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, usize n)
{
    char *d = dst;
    while (n && (*d = *src)) {
        d++;
        src++;
        n--;
    }
    while (n--)
        *d++ = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : 0;
}

static char *spStr(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *spDec(char *p, u32 v)
{
    char buf[12];
    int i = 0;
    if (v == 0) {
        *p++ = '0';
        return p;
    }
    while (v) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i--)
        *p++ = buf[i];
    return p;
}

static char *spHex(char *p, u32 v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[9];
    int i = 0;
    if (v == 0) {
        *p++ = '0';
        return p;
    }
    while (v) {
        buf[i++] = hex[v & 0xF];
        v >>= 4;
    }
    while (i--)
        *p++ = buf[i];
    return p;
}

static int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    char *p = buf;

    vaStart(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            *p++ = *fmt++;
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd': {
            i32 v = vaArg(ap, i32);
            if (v < 0) {
                *p++ = '-';
                v = -v;
            }
            p = spDec(p, (u32)v);
            break;
        }
        case 'u':
            p = spDec(p, vaArg(ap, u32));
            break;
        case 'x':
            p = spHex(p, vaArg(ap, u32));
            break;
        case 'c':
            *p++ = (char)vaArg(ap, int);
            break;
        case 's':
            p = spStr(p, vaArg(ap, const char *));
            break;
        case '%':
            *p++ = '%';
            break;
        default:
            *p++ = '%';
            *p++ = *fmt;
            break;
        }
        fmt++;
    }
    *p = 0;
    vaEnd(ap);
    return (int)(p - buf);
}

#endif
