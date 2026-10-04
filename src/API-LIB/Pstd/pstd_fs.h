#ifndef PSTD_FS_H
#define PSTD_FS_H

#include "pstd_int.h"

#define PFS_ERR (-1)
#define PFS_EPERM (-2)

#define USER_ROOT   0
#define USER_NORMAL 1

static FATFS pfs_vol;
static FIL   pfs_fil;
static u8    pfs_ready = 0;
static int   pfs_user = USER_ROOT;

static void pfs_set_user(int u) { pfs_user = u; }
static int  pfs_get_user(void)  { return pfs_user; }

static int pfs_in_system(const char *path)
{
    const char *root = "/SYSTEM";
    int i;

    if (!path || path[0] != '/')
        return 0;

    for (i = 0; root[i]; i++) {
        char c = path[i];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c != root[i])
            return 0;
    }
    return path[i] == 0 || path[i] == '/';
}

static int pfs_denied(const char *path)
{
    return pfs_user != USER_ROOT && pfs_in_system(path);
}

static int pfs_mount(void)
{
    if (pfs_ready)
        return 0;

    init_disk();

    if (f_mount(&pfs_vol, "0:", 1) != FR_OK)
        return PFS_ERR;

    pfs_ready = 1;
    return 0;
}

static int pfs_crt(const char *path)
{
    if (pfs_denied(path))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_open(&pfs_fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return PFS_ERR;

    if (f_close(&pfs_fil) != FR_OK)
        return PFS_ERR;

    return 0;
}

static int pfs_write(const char *path, const void *buf, u32 len)
{
    UINT bw = 0;

    if (pfs_denied(path))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_open(&pfs_fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return PFS_ERR;

    if (f_write(&pfs_fil, buf, (UINT)len, &bw) != FR_OK) {
        f_close(&pfs_fil);
        return PFS_ERR;
    }

    if (f_close(&pfs_fil) != FR_OK)
        return PFS_ERR;

    return (int)bw;
}

/* streaming writer: open once, append many chunks, close.  used by the
 * system installer which copies large files sector by sector. */
static int pfs_open_w(const char *path)
{
    if (pfs_denied(path))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_open(&pfs_fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return PFS_ERR;

    return 0;
}

static int pfs_put(const void *buf, u32 len)
{
    UINT bw = 0;

    if (f_write(&pfs_fil, buf, (UINT)len, &bw) != FR_OK || bw != len)
        return PFS_ERR;

    return 0;
}

static int pfs_close_w(void)
{
    return (f_close(&pfs_fil) == FR_OK) ? 0 : PFS_ERR;
}

static int pfs_read(const char *path, void *buf, u32 len)
{
    UINT br = 0;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_open(&pfs_fil, path, FA_READ) != FR_OK)
        return PFS_ERR;

    if (f_read(&pfs_fil, buf, (UINT)len, &br) != FR_OK) {
        f_close(&pfs_fil);
        return PFS_ERR;
    }

    if (f_close(&pfs_fil) != FR_OK)
        return PFS_ERR;

    return (int)br;
}

static int pfs_remove(const char *path)
{
    if (pfs_denied(path))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_unlink(path) != FR_OK)
        return PFS_ERR;

    return 0;
}

static int pfs_mkdir(const char *path)
{
    FRESULT r;

    if (pfs_denied(path))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    r = f_mkdir(path);
    return (r == FR_OK || r == FR_EXIST) ? 0 : PFS_ERR;
}

static int pfs_rmdir(const char *path)
{
    FILINFO fi;

    if (pfs_denied(path))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_stat(path, &fi) != FR_OK || !(fi.fattrib & AM_DIR))
        return PFS_ERR;

    return (f_unlink(path) == FR_OK) ? 0 : PFS_ERR;
}

/* list a directory into buf, one entry per line:
 *   "D <name>\n"  or  "F <name> <size>\n"
 * returns the number of entries, or PFS_ERR. */
static int pfs_list(const char *path, char *buf, u32 size)
{
    DIR dj;
    FILINFO fi;
    int n = 0;
    u32 used = 0;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_opendir(&dj, path) != FR_OK)
        return PFS_ERR;

    while (f_readdir(&dj, &fi) == FR_OK && fi.fname[0]) {
        int i;

        if (used + sizeof(fi.fname) + 16 >= size)
            break;

        buf[used++] = (fi.fattrib & AM_DIR) ? 'D' : 'F';
        buf[used++] = ' ';
        for (i = 0; fi.fname[i]; i++)
            buf[used++] = fi.fname[i];

        if (!(fi.fattrib & AM_DIR)) {
            char num[12];
            int k = 0;
            u32 v = (u32)fi.fsize;

            if (v == 0)
                num[k++] = '0';
            while (v) {
                num[k++] = (char)('0' + (v % 10u));
                v /= 10u;
            }
            buf[used++] = ' ';
            while (k)
                buf[used++] = num[--k];
        }

        buf[used++] = '\n';
        n++;
    }

    f_closedir(&dj);
    buf[used < size ? used : size - 1] = 0;
    return n;
}

static int pfs_copy(const char *src, const char *dst)
{
    static u8 cpbuf[512];
    FIL in, out;
    u32 total = 0;

    if (pfs_denied(dst))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_open(&in, src, FA_READ) != FR_OK)
        return PFS_ERR;

    if (f_open(&out, dst, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        f_close(&in);
        return PFS_ERR;
    }

    for (;;) {
        UINT br = 0, bw = 0;

        if (f_read(&in, cpbuf, sizeof(cpbuf), &br) != FR_OK) {
            f_close(&in);
            f_close(&out);
            return PFS_ERR;
        }
        if (br == 0)
            break;
        if (f_write(&out, cpbuf, br, &bw) != FR_OK || bw != br) {
            f_close(&in);
            f_close(&out);
            return PFS_ERR;
        }
        total += br;
    }

    f_close(&in);
    if (f_close(&out) != FR_OK)
        return PFS_ERR;

    return (int)total;
}

static int pfs_rename(const char *src, const char *dst)
{
    if (pfs_denied(src) || pfs_denied(dst))
        return PFS_EPERM;

    if (pfs_mount() != 0)
        return PFS_ERR;

    return (f_rename(src, dst) == FR_OK) ? 0 : PFS_ERR;
}

static int pfs_exists(const char *path)
{
    FILINFO fi;

    if (pfs_mount() != 0)
        return 0;

    return f_stat(path, &fi) == FR_OK;
}

static int pfs_size(const char *path)
{
    FILINFO fi;

    if (pfs_mount() != 0)
        return PFS_ERR;

    if (f_stat(path, &fi) != FR_OK)
        return PFS_ERR;

    if (fi.fattrib & AM_DIR)
        return PFS_ERR;

    return (int)fi.fsize;
}

#endif
