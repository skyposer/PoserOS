#ifndef PSTD_KLOG_H
#define PSTD_KLOG_H

#include "pstd_int.h"
#include "pstd_string.h"
#include "pstd_fs.h"

/* all runtime noise (syscall trace) goes into this one file; it is truncated
 * on every boot so it never grows without bound */
#define KLOG_FILE  "/system/syscall.log"
#define KLOG_MAX   (64u * 1024u)

static FIL  klog_fil;
static u32  klog_size;
static int  klog_on;

/* create/overwrite the log file; call once after the filesystem is mounted
 * and /system exists */
static void klog_init(void)
{
    klog_on   = 0;
    klog_size = 0;

    if (f_open(&klog_fil, KLOG_FILE, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return;
    f_close(&klog_fil);
    klog_on = 1;
}

/* append a line; never prints anything and never fails loudly */
static void klog(const char *s)
{
    UINT bw;
    u32  n = 0;

    if (!klog_on || klog_size >= KLOG_MAX)
        return;

    while (s[n])
        n++;
    if (n == 0 || klog_size + n + 1 > KLOG_MAX)
        return;

    if (f_open(&klog_fil, KLOG_FILE, FA_OPEN_APPEND | FA_WRITE) != FR_OK)
        return;

    if (f_write(&klog_fil, s, (UINT)n, &bw) == FR_OK &&
        f_write(&klog_fil, "\n", 1, &bw) == FR_OK)
        klog_size += n + 1;

    f_close(&klog_fil);
}

static void klog_syscall(u32 n, u32 a0, u32 a1, u32 a2)
{
    char b[88];

    sprintf(b, "syscall %u 0x%x 0x%x 0x%x", n, a0, a1, a2);
    klog(b);
}

#endif
