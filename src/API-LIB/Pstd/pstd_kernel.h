#ifndef PSTD_KERNEL_H
#define PSTD_KERNEL_H

#include "pstd_int.h"
#include "pstd_port.h"
#include "pstd_io.h"
#include "pstd_string.h"
#include "pstd_e820.h"
#include "pstd_pmm.h"

static void halt(void)
{
    __asm__ volatile("hlt");
}

static void hltsleep(void)
{
    for (;;)
        __asm__ volatile("hlt");
}

static void spin(void)
{
    for (;;)
        __asm__ volatile("pause");
}

static void cli(void)
{
    __asm__ volatile("cli");
}

static void sti(void)
{
    __asm__ volatile("sti");
}

static void nop(void)
{
    __asm__ volatile("nop");
}

static u32 readCr0(void)
{
    u32 v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static void writeCr0(u32 v)
{
    __asm__ volatile("mov %0, %%cr0" :: "r"(v));
}

static u32 readCr2(void)
{
    u32 v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static u32 readCr3(void)
{
    u32 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void writeCr3(u32 v)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(v));
}

static u32 readCr4(void)
{
    u32 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static void writeCr4(u32 v)
{
    __asm__ volatile("mov %0, %%cr4" :: "r"(v));
}

static u32 readEflags(void)
{
    u32 v;
    __asm__ volatile("pushfd\npop %0" : "=r"(v));
    return v;
}

static void writeEflags(u32 v)
{
    __asm__ volatile("push %0\npopfd" :: "r"(v));
}

static u64 readTsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void breakpoint(void)
{
    __asm__ volatile("int3");
}

static void memoryBarrier(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

static void ioBarrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static char *se_hex(char *p, u32 v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;

    for (i = digits - 1; i >= 0; i--) {
        p[i] = hx[v & 0xF];
        v >>= 4;
    }
    return p + digits;
}

static char *se_lab(char *p, const char *s, int width)
{
    int n = 0;

    while (*s) {
        *p++ = *s++;
        n++;
    }
    while (n++ < width)
        *p++ = ' ';
    return p;
}

static void se_reg(const char *name, u32 v, int digits)
{
    char buf[48];
    char *p = buf;

    p = se_lab(p, name, 8);
    *p++ = '0';
    *p++ = 'x';
    p = se_hex(p, v, digits);
    *p++ = '\n';
    *p = 0;
    ioprint(buf);
}

static u16 se_exitcode(const char *s)
{
    u32 h = 2166136261u;

    while (*s) {
        h ^= (u8)*s++;
        h *= 16777619u;
    }
    return (u16)(h ^ (h >> 16));
}

static void syserror(const char *reason)
{
    u32 eip, esp, ebp, eflags, tsc_hi, tsc_lo;
    u64 tsc;
    char buf[96];
    char *p;

    ioprint("\n): System ERROR\n");

    p = buf;
    p = spStr(p, "The errors are from ");
    p = spStr(p, reason);
    *p++ = '.';
    *p++ = '\n';
    *p = 0;
    ioprint(buf);

    ioprint("Please try to restart or try to fix this error.\n");
    ioprint("We cannot repair it automatically.\n");
    ioprint("Kernel's code is from Github.\n");
    ioprint("https://github.com/skyposer/PoserOS\n\n");

    p = buf;
    p = spStr(p, "EXIT CODE: 0x");
    p = se_hex(p, se_exitcode(reason), 4);
    *p++ = '\n';
    *p = 0;
    ioprint(buf);

    eip = (u32)(uptr)__builtin_return_address(0);
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
    eflags = readEflags();
    tsc = readTsc();
    tsc_hi = (u32)(tsc >> 32);
    tsc_lo = (u32)tsc;

    ioprint("--- debug info ---\n");
    se_reg("EIP", eip, 8);
    se_reg("ESP", esp, 8);
    se_reg("EBP", ebp, 8);
    se_reg("EFLAGS", eflags, 8);
    se_reg("CR0", readCr0(), 8);
    se_reg("CR2", readCr2(), 8);
    se_reg("CR3", readCr3(), 8);
    se_reg("CR4", readCr4(), 8);
    se_reg("TSC.HI", tsc_hi, 8);
    se_reg("TSC.LO", tsc_lo, 8);

    p = buf;
    p = spStr(p, "MEM     usable=");
    p = spDec(p, e820_total_bytes() >> 20);
    p = spStr(p, "MB frames=");
    p = spDec(p, pmm_frame_count);
    p = spStr(p, " free=");
    p = spDec(p, pmm_free_count());
    *p++ = '\n';
    *p = 0;
    ioprint(buf);

    p = buf;
    p = spStr(p, "DISK    sectors=");
    p = spDec(p, e820_disk_sectors());
    p = spStr(p, " (");
    p = spDec(p, e820_disk_sectors() >> 11);
    p = spStr(p, "MB)\n");
    *p = 0;
    ioprint(buf);

    ioprint("\n-- system halted --\n");
    cli();
    hltsleep();
}

static void reboot(void)
{
    u8 v;

    for (;;) {
        v = readp8(0x64);
        if (!(v & 0x02))
            break;
    }

    writep8(0x64, 0xFE);
    hltsleep();
}

static void shutdownQemu(void)
{
    writep16(0x604,  0x2000);
    writep16(0xB004, 0x2000);
    writep16(0x4004, 0x3400);
    hltsleep();
}

static u8 cmosRead(u8 reg)
{
    writep8(0x70, reg);
    return readp8(0x71);
}

static u8 cmosBcd(u8 v)
{
    return (u8)((v & 0x0F) + ((v >> 4) * 10));
}

static u32 rtcDosTime(void)
{
    u8 sec, min, hour, day, mon, year, regb;

    while (cmosRead(0x0A) & 0x80)
        ;

    sec  = cmosRead(0x00);
    min  = cmosRead(0x02);
    hour = cmosRead(0x04);
    day  = cmosRead(0x07);
    mon  = cmosRead(0x08);
    year = cmosRead(0x09);
    regb = cmosRead(0x0B);

    if (!(regb & 0x04)) {
        sec  = cmosBcd(sec);
        min  = cmosBcd(min);
        hour = cmosBcd((u8)(hour & 0x7F));
        day  = cmosBcd(day);
        mon  = cmosBcd(mon);
        year = cmosBcd(year);
    }

    return ((u32)(2000u + year - 1980u) << 25)
         | ((u32)(mon  & 0x0F) << 21)
         | ((u32)(day  & 0x1F) << 16)
         | ((u32)(hour & 0x1F) << 11)
         | ((u32)(min  & 0x3F) << 5)
         | ((u32)(sec  >> 1) & 0x1F);
}

#endif
