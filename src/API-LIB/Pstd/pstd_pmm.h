#ifndef PSTD_PMM_H
#define PSTD_PMM_H

#include "pstd_int.h"
#include "pstd_e820.h"

#define PMM_PAGE         4096u
#define PMM_MAX_FRAMES   (192u * 1024u)
#define PMM_KERNEL_LIMIT 0x04000000u

static u32 pmm_frame_phys[PMM_MAX_FRAMES];
static u8  pmm_frame_used[PMM_MAX_FRAMES / 8];
static u32 pmm_frame_count;
static u32 pmm_next;

static void pmm_init(void)
{
    u16 n = e820_count();
    u32 i, f;

    pmm_frame_count = 0;
    pmm_next = 0;

    for (i = 0; i < n && i < E820_MAX; i++) {
        e820_entry_t *e = &e820_table()[i];
        u64 base, len;

        if (e->type != E820_TYPE_USABLE)
            continue;

        base = (e->base + PMM_PAGE - 1) & ~((u64)PMM_PAGE - 1);
        if (base < e->base)
            continue;
        len = e->len - (base - e->base);
        len &= ~((u64)PMM_PAGE - 1);

        while (len >= PMM_PAGE && pmm_frame_count < PMM_MAX_FRAMES) {
            pmm_frame_phys[pmm_frame_count++] = (u32)base;
            base += PMM_PAGE;
            len  -= PMM_PAGE;
        }
    }

    for (f = 0; f < PMM_MAX_FRAMES / 8; f++)
        pmm_frame_used[f] = 0;

    for (f = 0; f < pmm_frame_count; f++)
        if (pmm_frame_phys[f] < PMM_KERNEL_LIMIT)
            pmm_frame_used[f >> 3] |= (u8)(1u << (f & 7));
}

static u32 pmm_alloc(void)
{
    u32 scanned, f = pmm_next;

    for (scanned = 0; scanned < pmm_frame_count; scanned++) {
        if (f >= pmm_frame_count)
            f = 0;
        if (!(pmm_frame_used[f >> 3] & (1u << (f & 7)))) {
            pmm_frame_used[f >> 3] |= (u8)(1u << (f & 7));
            pmm_next = f + 1;
            return pmm_frame_phys[f];
        }
        f++;
    }

    return 0;
}

static u32 pmm_index_of(u32 phys)
{
    u32 lo = 0, hi = pmm_frame_count;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (pmm_frame_phys[mid] < phys)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo < pmm_frame_count && pmm_frame_phys[lo] == phys)
        return lo;

    return 0xFFFFFFFFu;
}

static void pmm_free(u32 phys)
{
    u32 idx = pmm_index_of(phys);

    if (idx == 0xFFFFFFFFu || phys < PMM_KERNEL_LIMIT)
        return;

    pmm_frame_used[idx >> 3] &= (u8)~(1u << (idx & 7));
    if (idx < pmm_next)
        pmm_next = idx;
}

static u32 pmm_free_count(void)
{
    u32 f, c = 0;

    for (f = 0; f < pmm_frame_count; f++)
        if (!(pmm_frame_used[f >> 3] & (1u << (f & 7))))
            c++;

    return c;
}

#endif
