#ifndef PSTD_EXC_H
#define PSTD_EXC_H

#include "pstd_int.h"

#define PEXC_VERSION    0x0001u
#define PEXC_HDR_SIZE   64u

#define PEXC_ARCH_X86   0x86u
#define PEXC_BITS_32    32u

#define PEXC_PRIV_USER  0u
#define PEXC_PRIV_ROOT  1u

#define PEXC_FLAG_RING0 0x001u

#define PEXC_OK         0
#define PEXC_ENOENT    (-1)
#define PEXC_EIO       (-2)
#define PEXC_EBADMAGIC (-3)
#define PEXC_EBADHDR   (-4)
#define PEXC_EARCH     (-5)
#define PEXC_EMEM      (-6)
#define PEXC_EDISK     (-7)
#define PEXC_EPRIV     (-8)
#define PEXC_ESIZE     (-9)
#define PEXC_ECHECK    (-10)
#define PEXC_ENOMEM    (-11)

#define PEXC_CHK_OFF   56u

typedef struct {
    u8  magic[4];
    u16 version;
    u16 hdr_size;
    u32 arch;
    u32 bits;
    u32 created;
    u32 modified;
    u32 entry;
    u32 load;
    u32 code_size;
    u32 bss_size;
    u32 min_mem_mb;
    u32 min_disk_kb;
    u32 priv;
    u32 flags;
    u32 checksum;
    u32 reserved;
} __attribute__((packed)) pexc_header_t;

static u32 pexc_checksum(const u8 *hdr, const u8 *code, u32 len)
{
    u32 s = 2166136261u;
    u32 i;

    for (i = 0; i < PEXC_HDR_SIZE; i++) {
        u8 b = (i >= PEXC_CHK_OFF && i < PEXC_CHK_OFF + 4) ? 0u : hdr[i];
        s ^= b;
        s *= 16777619u;
    }
    for (i = 0; i < len; i++) {
        s ^= code[i];
        s *= 16777619u;
    }
    return s;
}

static int pexc_verify(const pexc_header_t *h, const u8 *code, u32 avail)
{
    if (h->magic[0] != 'P' || h->magic[1] != 'E' ||
        h->magic[2] != 'X' || h->magic[3] != 'C')
        return PEXC_EBADMAGIC;

    if (h->version != PEXC_VERSION || h->hdr_size != PEXC_HDR_SIZE)
        return PEXC_EBADHDR;

    if (h->arch != PEXC_ARCH_X86 || h->bits != PEXC_BITS_32)
        return PEXC_EARCH;

    if (h->code_size == 0 || h->code_size > avail)
        return PEXC_ESIZE;

    if (pexc_checksum((const u8 *)h, code, h->code_size) != h->checksum)
        return PEXC_ECHECK;

    return PEXC_OK;
}

#endif
