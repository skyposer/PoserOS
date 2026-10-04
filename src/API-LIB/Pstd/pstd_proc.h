#ifndef PSTD_PROC_H
#define PSTD_PROC_H

#include "pstd_int.h"
#include "pstd_port.h"
#include "pstd_kernel.h"
#include "pstd_string.h"
#include "pstd_paging.h"
#include "pstd_pmm.h"
#include "pstd_fs.h"
#include "pstd_kmalloc.h"
#include "pstd_gdt.h"
#include "pstd_idt.h"
#include "pstd_e820.h"
#include "pstd_exc.h"

#define PROC_MAX          8
#define PROC_NAME_LEN     12
#define PROC_STACK_PAGES  32
#define PROC_LOAD_MAX     (1024u * 1024u)
#define PROC_CR3_MAILBOX  0x5000u
#define PROC_SLOT         (USER_BASE + 8u * 1024u * 1024u)

#define PROC_DEAD         0
#define PROC_READY        1
#define PROC_WAIT         2
#define PROC_NEW          3
#define PROC_FREE         4

#define USER_ARG_VA       (USER_BASE + 0x00300000u)
#define USER_FRAME_VA     (USER_TOP - 0x100u)
#define USER_STACK_TOP    (USER_TOP - 0x200u)
#define USER_ARG_MAX      4000u

typedef struct {
    u32 edi, esi, ebp, esp0, ebx, edx, ecx, eax;
    u32 vec, err;
    u32 eip, cs, eflags, espf, ssf;
} proc_frame_t;

typedef struct {
    char name[PROC_NAME_LEN];
    u32  state;
    u32  cr3;
    u32  esp;
    u32  entry;
    u32  wake;
    u32  arg;
    u32  kstack;
    u32  load;
    u32  exit_code;
    int  parent;
    int  pid;
    u8   ring0;
} proc_t;

#define PROC_KSTACK_SIZE (8u * 1024u)
static u8 proc_kstack[PROC_MAX][PROC_KSTACK_SIZE] __attribute__((aligned(16)));

extern void proc_jump(u32 cr3, u32 esp);

static proc_t procs[PROC_MAX];
static int    proc_count;
static int    proc_cur = -1;
static u32    proc_on;

static proc_t *proc_create(const char *name)
{
    proc_t *p;
    int idx = -1;
    u32 pd, i, va;

    /* reuse a reaped slot, otherwise take the next never-used one */
    for (i = 0; i < PROC_MAX; i++) {
        if (i >= (u32)proc_count || procs[i].state == PROC_FREE) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0)
        return 0;

    pd = paging_new_dir();
    if (!pd)
        return 0;

    p = &procs[idx];
    for (i = 0; i < PROC_NAME_LEN - 1 && name[i]; i++)
        p->name[i] = name[i];
    p->name[i] = 0;

    p->state     = PROC_NEW;
    p->cr3       = pd;
    p->esp       = 0;
    p->entry     = 0;
    p->wake      = 0;
    p->arg       = 0;
    p->kstack    = (u32)proc_kstack[idx] + PROC_KSTACK_SIZE;
    p->load      = USER_BASE;
    p->exit_code = 0;
    p->parent    = proc_cur;
    p->pid       = idx;
    p->ring0     = 0;

    for (i = 0; i < PROC_STACK_PAGES; i++) {
        va = USER_TOP - (i + 1) * 4096u;
        if (!paging_user_alloc(pd, va))
            return 0;
    }

    if (!paging_user_alloc(pd, USER_ARG_VA))
        return 0;

    if (idx == proc_count)
        proc_count++;

    return p;
}

static int proc_load_image(proc_t *p, const u8 *code, u32 size, u32 bss)
{
    u32 off, ph, chunk, total;

    if (!size)
        return -1;

    total = size + bss;
    for (off = 0; off < total; off += 4096u) {
        ph = paging_user_alloc(p->cr3, p->load + off);
        if (!ph)
            return -1;

        chunk = total - off;
        if (chunk > 4096u)
            chunk = 4096u;

        if (off < size) {
            u32 c = size - off;
            if (c > chunk)
                c = chunk;
            memcpy(kmaphys(ph), code + off, c);
            if (c < chunk)
                memset((u8 *)kmaphys(ph) + c, 0, chunk - c);
        } else {
            memset(kmaphys(ph), 0, chunk);
        }
    }
    return (int)size;
}

static int proc_load(proc_t *p, const char *file)
{
    u8 *tmp;
    int n, sz, r;

    sz = pfs_size(file);
    if (sz <= 0 || (u32)sz > PROC_LOAD_MAX)
        return -1;

    tmp = kmalloc((u32)sz);
    if (!tmp)
        return -1;

    n = pfs_read(file, tmp, (u32)sz);
    if (n <= 0) {
        kfree(tmp);
        return -1;
    }

    r = proc_load_image(p, tmp, (u32)n, 0);
    kfree(tmp);
    if (r < 0)
        return -1;

    p->load  = USER_BASE;
    p->entry = USER_BASE;
    return n;
}

static proc_t *proc_create_fn(const char *name, void (*fn)(void))
{
    proc_t *p = proc_create(name);

    if (p)
        p->entry = (u32)fn;

    return p;
}

static void proc_boot(proc_t *p)
{
    u32 ph = paging_lookup(p->cr3, USER_FRAME_VA);
    u32 *f;
    u32 i;

    if (!ph)
        return;

    f = (u32 *)((u8 *)kmaphys(ph) + (USER_FRAME_VA & 0xFFFu));

    for (i = 0; i < 8; i++)
        f[i] = 0;

    f[7]  = p->arg;
    f[8]  = 0;
    f[9]  = 0;
    f[10] = p->entry;
    f[12] = 0x202;

    if (p->ring0) {
        f[11] = SEG_KCODE;
    } else {
        f[11] = SEG_UCODE3;
        f[13] = USER_STACK_TOP;
        f[14] = SEG_UDATA3;
    }

    p->esp = USER_FRAME_VA;

    /* only now is the process safe for the scheduler to pick: its initial
     * frame and stack pointer are in place.  Publishing it earlier would let
     * a timer tick preempt proc_exec() and switch to a half-built process. */
    p->state = PROC_READY;
}

/* Release a finished process's address space and return its table slot to the
 * pool.  This runs in the parent (reaper) context, never in the dying process:
 * by the time the parent observes PROC_DEAD the scheduler has already switched
 * away from the child, so none of the frames being freed are still mapped. */
static void proc_reap(int pid)
{
    proc_t *p = &procs[pid];

    if (p->cr3)
        paging_free_dir(p->cr3);

    p->cr3       = 0;
    p->esp       = 0;
    p->arg       = 0;
    p->exit_code = 0;
    p->state     = PROC_FREE;
}

static int proc_copy_in(proc_t *p, u32 dst_va, const void *src, u32 len)
{
    u32 off = 0;

    while (off < len) {
        u32 va = dst_va + off;
        u32 ph = paging_lookup(p->cr3, va);
        u32 page_off, chunk;

        if (!ph)
            return -1;

        page_off = va & 0xFFFu;
        chunk = 4096u - page_off;
        if (chunk > len - off)
            chunk = len - off;

        memcpy((u8 *)kmaphys(ph) + page_off, (const u8 *)src + off, chunk);
        off += chunk;
    }

    return (int)len;
}

static int proc_exec(const char *path, const char *args, u32 want_ring0)
{
    pexc_header_t hdr;
    u8 *img;
    proc_t *p;
    u32 argsz, n;
    int sz, rc, ring0;

    sz = pfs_size(path);
    if (sz <= 0)
        return PEXC_ENOENT;

    if ((u32)sz > PROC_LOAD_MAX || (u32)sz < PEXC_HDR_SIZE)
        return PEXC_ESIZE;

    img = kmalloc((u32)sz);
    if (!img)
        return PEXC_ENOMEM;

    n = (u32)pfs_read(path, img, (u32)sz);
    if (n < PEXC_HDR_SIZE) {
        kfree(img);
        return PEXC_EIO;
    }

    memcpy(&hdr, img, PEXC_HDR_SIZE);
    rc = pexc_verify(&hdr, img + PEXC_HDR_SIZE, n - PEXC_HDR_SIZE);
    if (rc != PEXC_OK) {
        kfree(img);
        return rc;
    }

    if (hdr.min_mem_mb && (e820_total_bytes() >> 20) < hdr.min_mem_mb) {
        kfree(img);
        return PEXC_EMEM;
    }

    if (hdr.min_disk_kb && ((e820_disk_sectors() * 512u) >> 10) < hdr.min_disk_kb) {
        kfree(img);
        return PEXC_EDISK;
    }

    if (hdr.priv == PEXC_PRIV_ROOT && pfs_get_user() != USER_ROOT) {
        kfree(img);
        return PEXC_EPRIV;
    }

    ring0 = (want_ring0 && pfs_get_user() == USER_ROOT &&
             (hdr.flags & PEXC_FLAG_RING0)) ? 1 : 0;

    p = proc_create(ring0 ? "root" : "user");
    if (!p) {
        kfree(img);
        return PEXC_ENOMEM;
    }

    p->load = hdr.load ? hdr.load : USER_BASE;

    if (proc_load_image(p, img + PEXC_HDR_SIZE, hdr.code_size, hdr.bss_size) < 0) {
        proc_reap(p->pid);
        kfree(img);
        return PEXC_EIO;
    }
    kfree(img);

    p->entry = hdr.entry;
    p->ring0 = (u8)ring0;

    argsz = 0;
    if (args) {
        while (args[argsz] && argsz < USER_ARG_MAX)
            argsz++;
    }

    if (proc_copy_in(p, USER_ARG_VA, args ? args : "", argsz + 1) < 0) {
        proc_reap(p->pid);
        return PEXC_EIO;
    }

    p->arg = USER_ARG_VA;
    proc_boot(p);
    return p->pid;
}

static int proc_wait(int pid)
{
    proc_t *p;
    int code;

    if (pid < 0 || pid >= proc_count)
        return -1;

    p = &procs[pid];

    if (p->state == PROC_FREE)
        return -1;

    if (p->state == PROC_DEAD) {
        code = (int)p->exit_code;
        proc_reap(pid);
        return code;
    }

    if (proc_cur >= 0) {
        procs[proc_cur].state = PROC_WAIT;
        procs[proc_cur].wake  = 0;
    }

    while (p->state != PROC_DEAD)
        __asm__ volatile("sti; hlt");

    if (proc_cur >= 0)
        procs[proc_cur].state = PROC_READY;

    code = (int)p->exit_code;
    proc_reap(pid);
    return code;
}

static proc_t *proc_pick(void)
{
    int n, i;

    for (n = 1; n <= proc_count; n++) {
        i = proc_cur + n;
        while (i < 0)
            i += proc_count;
        i %= proc_count;

        if (procs[i].state == PROC_READY && procs[i].wake <= timer_ticks)
            return &procs[i];
    }

    return 0;
}

static u32 proc_schedule(u32 cur_esp)
{
    proc_t *p;

    if (!proc_on)
        return 0;

    p = proc_pick();
    if (!p)
        return 0;
    if (proc_cur >= 0 && p == &procs[proc_cur])
        return 0;

    if (proc_cur >= 0)
        procs[proc_cur].esp = cur_esp;

    proc_cur = (int)(p - procs);
    *(volatile u32 *)PROC_CR3_MAILBOX = p->cr3;
    gdt_set_kstack(p->kstack);
    return p->esp;
}

static void proc_start(void)
{
    int i;

    for (i = 0; i < proc_count; i++)
        if (procs[i].state == PROC_READY)
            break;

    if (i >= proc_count)
        return;

    cli();
    proc_cur = i;
    proc_on  = 1;
    gdt_set_kstack(procs[i].kstack);
    proc_jump(procs[i].cr3, procs[i].esp);
}

static void proc_sleep(u32 ticks)
{
    u32 until = timer_ticks + ticks;

    if (proc_cur >= 0)
        procs[proc_cur].wake = until;

    while ((i32)(timer_ticks - until) < 0)
        __asm__ volatile("sti; hlt");

    if (proc_cur >= 0)
        procs[proc_cur].wake = 0;
}

static void proc_exit(u32 code)
{
    int me = proc_cur;

    if (me >= 0) {
        procs[me].state     = PROC_DEAD;
        procs[me].exit_code = code;

        if (procs[me].parent >= 0 && procs[me].parent < proc_count) {
            procs[procs[me].parent].state = PROC_READY;
            procs[procs[me].parent].wake  = 0;
        }
    }

    for (;;)
        __asm__ volatile("sti; hlt");
}

#endif
