#ifndef PSTD_PORT_H
#define PSTD_PORT_H

#include "pstd_int.h"

static inline void writep8(u16 port, u8 val)
{
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline u8 readp8(u16 port)
{
    u8 ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void writep16(u16 port, u16 val)
{
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static inline u16 readp16(u16 port)
{
    u16 ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void writep32(u16 port, u32 val)
{
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}

static inline u32 readp32(u16 port)
{
    u32 ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void pwait(void)
{
    writep8(0x80, 0);
}

#endif