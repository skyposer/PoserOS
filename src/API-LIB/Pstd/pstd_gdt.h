#ifndef PSTD_GDT_H
#define PSTD_GDT_H

#include "pstd_int.h"

#define SEG_KCODE  0x08
#define SEG_KDATA  0x10
#define SEG_UCODE  0x18
#define SEG_UDATA  0x20
#define SEG_TSS    0x28

#define SEG_UCODE3 (SEG_UCODE | 3)
#define SEG_UDATA3 (SEG_UDATA | 3)

#define GDT_ENTRIES 6

typedef struct {
    u32 prev_tss;
    u32 esp0;
    u32 ss0;
    u32 esp1, ss1;
    u32 esp2, ss2;
    u32 cr3;
    u32 eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap;
    u16 iomap_base;
} __attribute__((packed)) tss_t;

typedef struct {
    u16 limit;
    u32 base;
} __attribute__((packed)) gdtr_t;

static u64    gdt[GDT_ENTRIES];
static gdtr_t gdtr;
static tss_t  tss;

static u8 gdt_kstack_default[16 * 1024] __attribute__((aligned(16)));

static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 flags)
{
    gdt[i] = (u64)(limit & 0xFFFF)
           | ((u64)(base & 0xFFFFFF) << 16)
           | ((u64)access << 40)
           | ((u64)((limit >> 16) & 0x0F) << 48)
           | ((u64)(flags & 0x0F) << 52)
           | ((u64)((base >> 24) & 0xFF) << 56);
}

static void gdt_set_kstack(u32 top)
{
    tss.esp0 = top;
}

static void gdt_init(void)
{
    u32 i;

    for (i = 0; i < sizeof(tss); i++)
        ((u8 *)&tss)[i] = 0;

    tss.ss0        = SEG_KDATA;
    tss.esp0       = (u32)gdt_kstack_default + sizeof(gdt_kstack_default);
    tss.iomap_base = (u16)sizeof(tss);

    gdt_set(0, 0, 0,          0x00, 0x0);
    gdt_set(1, 0, 0x000FFFFF, 0x9A, 0xC);
    gdt_set(2, 0, 0x000FFFFF, 0x92, 0xC);
    gdt_set(3, 0, 0x000FFFFF, 0xFA, 0xC);
    gdt_set(4, 0, 0x000FFFFF, 0xF2, 0xC);
    gdt_set(5, (u32)&tss, (u32)sizeof(tss) - 1, 0x89, 0x0);

    gdtr.limit = (u16)(sizeof(gdt) - 1);
    gdtr.base  = (u32)gdt;

    __asm__ volatile("lgdt %0" ::"m"(gdtr));

    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        ::: "eax", "memory");

    {
        u16 sel = SEG_TSS;
        __asm__ volatile("ltr %0" ::"r"(sel));
    }
}

#endif
