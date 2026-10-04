#ifndef PSTD_E820_H
#define PSTD_E820_H

#include "pstd_int.h"

#define BOOTINFO_ADDR    0x8000
#define E820_ADDR        (BOOTINFO_ADDR + 12)
#define E820_MAX         128
#define E820_TYPE_USABLE 1

typedef struct {
    u64 base;
    u64 len;
    u32 type;
    u32 acpi;
} e820_entry_t;

static e820_entry_t *e820_table(void)
{
    return (e820_entry_t *)E820_ADDR;
}

static u16 e820_count(void)
{
    return *(volatile u16 *)BOOTINFO_ADDR;
}

static u32 e820_total_bytes(void)
{
    return *(volatile u32 *)(BOOTINFO_ADDR + 4);
}

static u32 e820_disk_sectors(void)
{
    return *(volatile u32 *)(BOOTINFO_ADDR + 8);
}

#endif
