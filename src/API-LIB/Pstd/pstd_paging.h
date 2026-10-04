#ifndef PSTD_PAGING_H
#define PSTD_PAGING_H

#include "pstd_int.h"
#include "pstd_kernel.h"
#include "pstd_pmm.h"

#define PG_P   0x001u
#define PG_RW  0x002u
#define PG_U   0x004u
#define PG_PS  0x080u

#define PG_IDENT_PDES   16u
#define PG_IDENT_LIMIT  0x04000000u

#define KMAP_BASE       0x20000000u
#define KMAP_PDE        (KMAP_BASE >> 22)
#define KMAP_PDES       192u
#define KMAP_SIZE       (KMAP_PDES << 22)

#define USER_BASE       0x04000000u
#define USER_SIZE       (16u * 1024u * 1024u)
#define USER_TOP        (USER_BASE + USER_SIZE)
#define USER_PDES       (USER_SIZE >> 22)

#define kmaphys(p)      ((void *)(KMAP_BASE + (u32)(p)))

static u32 pg_dir[1024]                     __attribute__((aligned(4096)));
static u32 pg_pt_ident[PG_IDENT_PDES][1024] __attribute__((aligned(4096)));

static u32 pg_user_frames;

static void paging_map_identity(void)
{
    u32 pde, i;

    for (pde = 0; pde < 1024; pde++)
        pg_dir[pde] = 0;

    for (pde = 0; pde < PG_IDENT_PDES; pde++) {
        u32 base = pde << 22;
        for (i = 0; i < 1024; i++)
            pg_pt_ident[pde][i] = (base + (i << 12)) | PG_P | PG_RW;
        pg_dir[pde] = ((u32)pg_pt_ident[pde]) | PG_P | PG_RW;
    }
}

static void paging_map_kmap(void)
{
    u32 i;

    for (i = 0; i < KMAP_PDES; i++)
        pg_dir[KMAP_PDE + i] = (i << 22) | PG_P | PG_RW | PG_PS;
}

static void paging_kernel_part(u32 *d)
{
    u32 i;

    for (i = 0; i < PG_IDENT_PDES; i++)
        d[i] = pg_dir[i];

    for (i = 0; i < KMAP_PDES; i++)
        d[KMAP_PDE + i] = pg_dir[KMAP_PDE + i];
}

static void paging_enable(void)
{
    writeCr4(readCr4() | (1u << 4));
    writeCr3((u32)pg_dir);
    writeCr0(readCr0() | 0x80000000u);
}

static void paging_init(void)
{
    pg_user_frames = 0;
    paging_map_identity();
    paging_map_kmap();
    paging_enable();
}

static u32 paging_new_dir(void)
{
    u32 phys = pmm_alloc();
    u32 *d;
    u32 i;

    if (!phys)
        return 0;

    d = (u32 *)kmaphys(phys);

    for (i = 0; i < 1024; i++)
        d[i] = 0;

    paging_kernel_part(d);
    return phys;
}

static int paging_map(u32 pd_phys, u32 virt, u32 phys)
{
    u32 *pd = (u32 *)kmaphys(pd_phys);
    u32 pdi = virt >> 22;
    u32 pti = (virt >> 12) & 0x3FF;
    u32 *pt;
    u32 i;

    if (pdi < (USER_BASE >> 22) || pdi >= KMAP_PDE)
        return -1;

    if (pd[pdi] & PG_P) {
        pt = (u32 *)kmaphys(pd[pdi] & ~0xFFFu);
    } else {
        u32 t = pmm_alloc();

        if (!t)
            return -1;

        pt = (u32 *)kmaphys(t);
        for (i = 0; i < 1024; i++)
            pt[i] = 0;
        pd[pdi] = t | PG_P | PG_RW | PG_U;
    }

    pt[pti] = (phys & ~0xFFFu) | PG_P | PG_RW | PG_U;
    pg_user_frames++;
    return 0;
}

static u32 paging_lookup(u32 pd_phys, u32 virt)
{
    u32 *pd = (u32 *)kmaphys(pd_phys);
    u32 pdi = virt >> 22;
    u32 *pt;

    if (!(pd[pdi] & PG_P))
        return 0;

    pt = (u32 *)kmaphys(pd[pdi] & ~0xFFFu);
    return pt[(virt >> 12) & 0x3FF] & ~0xFFFu;
}

static u32 paging_user_alloc(u32 pd_phys, u32 virt)
{
    u32 phys = pmm_alloc();

    if (!phys)
        return 0;

    if (paging_map(pd_phys, virt, phys) != 0)
        return 0;

    return phys;
}

/* release every frame that belongs to a user address space: all mapped user
 * pages, the page tables that map them, and finally the page directory itself.
 * only the user PDE window is touched, so the shared kernel identity/kmap
 * mappings are left alone. */
static void paging_free_dir(u32 pd_phys)
{
    u32 *pd = (u32 *)kmaphys(pd_phys);
    u32 pdi, pti;

    if (!pd_phys)
        return;

    for (pdi = (USER_BASE >> 22); pdi < KMAP_PDE; pdi++) {
        u32 *pt;

        if (!(pd[pdi] & PG_P))
            continue;

        pt = (u32 *)kmaphys(pd[pdi] & ~0xFFFu);
        for (pti = 0; pti < 1024; pti++) {
            if (pt[pti] & PG_P) {
                pmm_free(pt[pti] & ~0xFFFu);
                if (pg_user_frames)
                    pg_user_frames--;
            }
        }

        pmm_free(pd[pdi] & ~0xFFFu);
        pd[pdi] = 0;
    }

    pmm_free(pd_phys);
}

static void paging_switch(u32 pd_phys)
{
    writeCr3(pd_phys);
}

static void paging_invlpg(u32 virt)
{
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

#endif
