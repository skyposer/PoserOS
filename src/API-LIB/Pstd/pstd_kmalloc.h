#ifndef PSTD_KMALLOC_H
#define PSTD_KMALLOC_H

#include "pstd_int.h"

extern u8 __kernel_heap_start[];
extern u8 __kernel_heap_end[];

#define KHEAP_NBINS   64
#define KHEAP_ALIGN   8
#define KHEAP_HDR     8
#define KHEAP_MINBLK  16

#define KHEAP_CEIL    0x04000000u
#define KHEAP_FLOOR   0x00040000u

#define KH_INUSE      0x1u
#define KH_PREVUSE    0x2u

typedef struct kheap_blk {
    u32 prev_size;
    u32 size_flags;
} kheap_blk_t;

typedef struct kheap_free {
    u32 prev_size;
    u32 size_flags;
    u32 next;
    u32 prev;
} kheap_free_t;

#define KH_SZ(b)        ((b)->size_flags & ~(u32)7)
#define KH_SET_SZ(b, v) ((b)->size_flags = ((b)->size_flags & (KH_INUSE | KH_PREVUSE)) | (u32)(v))
#define KH_NEXT(b)      ((kheap_blk_t *)((u8 *)(b) + KH_SZ(b)))

static kheap_blk_t *kheap_bins[KHEAP_NBINS];
static u64          kheap_binmap;
static u8           kheap_inited;
static u32          kheap_want;

static void kheap_set_size(u32 bytes)
{
    kheap_want = bytes;
}

static int kh_bin(u32 sz)
{
    int b = 0;

    sz >>= 4;
    while (sz > 1) {
        sz >>= 1;
        b++;
    }
    return (b < KHEAP_NBINS) ? b : KHEAP_NBINS - 1;
}

static void kh_binsert(kheap_blk_t *b)
{
    int i = kh_bin(KH_SZ(b));
    kheap_free_t *f = (kheap_free_t *)b;

    f->prev = 0;
    f->next = (u32)(uptr)kheap_bins[i];
    if (kheap_bins[i])
        ((kheap_free_t *)kheap_bins[i])->prev = (u32)(uptr)b;

    kheap_bins[i] = b;
    kheap_binmap |= (1ull << i);
}

static void kh_bremove(kheap_blk_t *b)
{
    int i = kh_bin(KH_SZ(b));
    kheap_free_t *f = (kheap_free_t *)b;

    if (f->prev)
        ((kheap_free_t *)(uptr)f->prev)->next = f->next;
    else
        kheap_bins[i] = (kheap_blk_t *)(uptr)f->next;

    if (f->next)
        ((kheap_free_t *)(uptr)f->next)->prev = f->prev;

    if (!kheap_bins[i])
        kheap_binmap &= ~(1ull << i);
}

static kheap_blk_t *kh_find(u32 need)
{
    u64 m = kheap_binmap & (~0ull << kh_bin(need));

    while (m) {
        int i = __builtin_ctzll(m);
        kheap_blk_t *cur = kheap_bins[i];

        while (cur) {
            if (KH_SZ(cur) >= need)
                return cur;
            cur = (kheap_blk_t *)(uptr)((kheap_free_t *)cur)->next;
        }
        m &= m - 1;
    }
    return 0;
}

static kheap_blk_t *kh_split(kheap_blk_t *b, u32 need)
{
    u32 s = KH_SZ(b);
    kheap_blk_t *r, *nxt;

    if (s < need + KHEAP_MINBLK)
        return b;

    r = (kheap_blk_t *)((u8 *)b + need);
    r->prev_size = need;
    r->size_flags = (s - need) | KH_PREVUSE;

    KH_SET_SZ(b, need);

    nxt = KH_NEXT(r);
    nxt->prev_size = KH_SZ(r);

    kh_binsert(r);
    return b;
}

static void kheap_init(void)
{
    uptr base = (uptr)__kernel_heap_start;
    uptr top;
    uptr ceil = (uptr)KHEAP_CEIL;
    kheap_blk_t *b, *epi;
    u32 total, i, want;

    for (i = 0; i < KHEAP_NBINS; i++)
        kheap_bins[i] = 0;
    kheap_binmap = 0;
    kheap_inited = 1;

    want = kheap_want ? kheap_want : (u32)(ceil - base);
    if (want < KHEAP_FLOOR)
        want = KHEAP_FLOOR;

    top = base + want;
    if (top > ceil)
        top = ceil;

    base = (base + (KHEAP_ALIGN - 1)) & ~(uptr)(KHEAP_ALIGN - 1);
    top  = top & ~(uptr)(KHEAP_ALIGN - 1);
    if (top <= base + KHEAP_MINBLK + KHEAP_HDR)
        return;

    total = (u32)(top - base);

    b = (kheap_blk_t *)base;
    b->prev_size = 0;
    b->size_flags = (total - KHEAP_HDR) | KH_PREVUSE;

    epi = KH_NEXT(b);
    epi->prev_size = KH_SZ(b);
    epi->size_flags = KHEAP_HDR | KH_INUSE;

    kh_binsert(b);
}

static void *kmalloc(u32 size)
{
    kheap_blk_t *b, *nxt;
    u32 need;

    if (!kheap_inited)
        kheap_init();
    if (!size)
        return 0;

    need = (size + KHEAP_HDR + (KHEAP_ALIGN - 1)) & ~(u32)(KHEAP_ALIGN - 1);
    if (need < KHEAP_MINBLK)
        need = KHEAP_MINBLK;

    b = kh_find(need);
    if (!b)
        return 0;

    kh_bremove(b);
    b = kh_split(b, need);

    b->size_flags |= KH_INUSE;
    nxt = KH_NEXT(b);
    nxt->size_flags |= KH_PREVUSE;

    return (u8 *)b + KHEAP_HDR;
}

static void kfree(void *p)
{
    kheap_blk_t *b, *nxt, *prv;

    if (!p)
        return;

    b = (kheap_blk_t *)((u8 *)p - KHEAP_HDR);

    nxt = KH_NEXT(b);
    if (!(nxt->size_flags & KH_INUSE)) {
        kh_bremove(nxt);
        KH_SET_SZ(b, KH_SZ(b) + KH_SZ(nxt));
    }

    if (!(b->size_flags & KH_PREVUSE)) {
        prv = (kheap_blk_t *)((u8 *)b - b->prev_size);
        kh_bremove(prv);
        KH_SET_SZ(prv, KH_SZ(prv) + KH_SZ(b));
        b = prv;
    }

    b->size_flags &= ~(u32)KH_INUSE;
    nxt = KH_NEXT(b);
    nxt->prev_size = KH_SZ(b);
    nxt->size_flags &= ~(u32)KH_PREVUSE;
    kh_binsert(b);
}

#endif
