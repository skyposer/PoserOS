#ifndef PSTD_IDT_H
#define PSTD_IDT_H

#include "pstd_int.h"
#include "pstd_port.h"
#include "pstd_kernel.h"
#include "pstd_io.h"
#include "pstd_string.h"

#define IDT_ENTRIES 256

#define PIC1_CMD  0x20
#define PIC1_DAT  0x21
#define PIC2_CMD  0xA0
#define PIC2_DAT  0xA1

#define IRQ_TIMER 0
#define IRQ_KB    1

typedef struct {
    u16 off_lo;
    u16 sel;
    u8  zero;
    u8  attr;
    u16 off_hi;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    u16 limit;
    u32 base;
} __attribute__((packed)) idtr_t;

typedef struct {
    u32 edi, esi, ebp, esp0, ebx, edx, ecx, eax;
    u32 vec, err;
    u32 eip, cs, eflags;
} intr_frame_t;

extern u32 intr_stub_table[];
extern void isr128(void);

static u32 proc_schedule(u32 cur_esp);

static u32 syscall_dispatch(u32 n, u32 a0, u32 a1, u32 a2);

static idt_entry_t idt[IDT_ENTRIES];
static idtr_t idtr;
static volatile u32 timer_ticks;

static volatile u32 syscall_active;

static const char *const exc_name[32] = {
    [0]  = "divide error",
    [1]  = "debug",
    [2]  = "nmi",
    [3]  = "breakpoint",
    [4]  = "overflow",
    [5]  = "bound range",
    [6]  = "invalid opcode",
    [7]  = "device not available",
    [8]  = "double fault",
    [9]  = "coprocessor segment overrun",
    [10] = "invalid tss",
    [11] = "segment not present",
    [12] = "stack fault",
    [13] = "general protection",
    [14] = "page fault",
    [15] = "reserved",
    [16] = "x87 fpu",
    [17] = "alignment check",
    [18] = "machine check",
    [19] = "simd fpu",
};

static void idt_set(int n, u32 handler, u8 attr)
{
    idt[n].off_lo = (u16)(handler & 0xFFFF);
    idt[n].sel    = 0x08;
    idt[n].zero   = 0;
    idt[n].attr   = attr;
    idt[n].off_hi = (u16)((handler >> 16) & 0xFFFF);
}

static void pic_remap(void)
{
    writep8(PIC1_CMD, 0x11); writep8(0x80, 0);
    writep8(PIC2_CMD, 0x11); writep8(0x80, 0);
    writep8(PIC1_DAT, 0x20); writep8(0x80, 0);
    writep8(PIC2_DAT, 0x28); writep8(0x80, 0);
    writep8(PIC1_DAT, 0x04); writep8(0x80, 0);
    writep8(PIC2_DAT, 0x02); writep8(0x80, 0);
    writep8(PIC1_DAT, 0x01); writep8(0x80, 0);
    writep8(PIC2_DAT, 0x01); writep8(0x80, 0);
}

static void pic_mask(u8 master, u8 slave)
{
    writep8(PIC1_DAT, master);
    writep8(PIC2_DAT, slave);
}

static void pit_init(u32 hz)
{
    u32 div = 1193182u / hz;

    writep8(0x43, 0x36);
    writep8(0x40, (u8)(div & 0xFF));
    writep8(0x40, (u8)((div >> 8) & 0xFF));
}

__attribute__((used)) u32 intr_dispatch(intr_frame_t *f)
{
    char buf[96];

    if (f->vec < 32) {

        ioprintln("");
        ioprint("EXCEPTION: ");
        ioprintln(exc_name[f->vec] ? exc_name[f->vec] : "unknown");

        sprintf(buf, "  vec=%d err=0x%x eip=0x%x cs=0x%x",
                f->vec, f->err, f->eip, f->cs);
        ioprintln(buf);

        if (f->vec == 14) {
            sprintf(buf, "  cr2=0x%x (faulting address)", readCr2());
            ioprintln(buf);
        }

        cli();
        hltsleep();
    }

    if (f->vec == 0x80) {

        syscall_active = 1;
        sti();
        f->eax = syscall_dispatch(f->eax, f->ebx, f->ecx, f->edx);
        syscall_active = 0;
        return 0;
    }

    if (f->vec == 32 + IRQ_TIMER) {
        u32 next = 0;

        timer_ticks++;
        writep8(PIC1_CMD, 0x20);

        /* preempt on every tick, including kernel (ring0) wait loops, so a
         * process blocked in proc_wait() lets its children run */
        next = proc_schedule((u32)f);
        return next;
    }

    if (f->vec == 32 + IRQ_KB)
        kb_feed(readp8(KB_DATA));

    if (f->vec >= 40)
        writep8(PIC2_CMD, 0x20);
    writep8(PIC1_CMD, 0x20);
    return 0;
}

static void idt_init(void)
{
    int i;

    for (i = 0; i < 48; i++)
        idt_set(i, intr_stub_table[i], 0x8E);

    idt_set(0x80, (u32)isr128, 0xEE);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (u32)idt;
    __asm__ volatile("lidt %0" ::"m"(idtr));

    pic_remap();
    pic_mask(0xFC, 0xFF);
    pit_init(100);
}

#endif
