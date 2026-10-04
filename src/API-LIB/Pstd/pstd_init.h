#ifndef PSTD_INIT_H
#define PSTD_INIT_H

#include "pstd_int.h"
#include "pstd_io.h"
#include "pstd_kernel.h"
#include "pstd_e820.h"
#include "pstd_pmm.h"
#include "pstd_paging.h"
#include "pstd_idt.h"
#include "pstd_fs.h"
#include "pstd_klog.h"
#include "pstd_kmalloc.h"
#include "pstd_gdt.h"
#include "pstd_proc.h"
#include "pstd_syscall.h"

#define SYS_USER_MEM_MIN  (64u * 1024u * 1024u)
#define SYS_DISK_MIN      (1u  * 1024u * 1024u)
#define SYS_HEAP_MIN      (256u * 1024u)
#define SYS_HEAP_PERCENT  8u

#define SHELL_FILE        "shell.bin"
#define BOOTBUF_SIZE      4096

#define BOOT_ARG_VA       USER_ARG_VA

static char boot_buf[BOOTBUF_SIZE];

static proc_t *shell_proc;

static u32 user_mem_bytes(void)
{
    u32 t = e820_total_bytes();

    return (t > 0x04000000u) ? (t - 0x04000000u) : 0u;
}

static void init_memory(void)
{
    u32 heap;
    char line[96];

    pmm_init();

    heap = user_mem_bytes() * SYS_HEAP_PERCENT / 100u;
    if (heap < SYS_HEAP_MIN)
        heap = SYS_HEAP_MIN;
    kheap_set_size(heap);

    sprintf(line, "heap: %u KB = %u%% of %u MB user memory",
            heap >> 10, SYS_HEAP_PERCENT, user_mem_bytes() >> 20);
    ioprintln(line);

    paging_init();
    gdt_init();
    idt_init();
}

/* old images stored 8.3 upper-case names; rename them in place so an existing
 * disk switches to lower-case names without losing any data. */
static void fix_names(void)
{
    static const char *ren[][2] = {
        { "/SYSTEM",              "/system" },
        { "/system/BIN",          "/system/bin" },
        { "/system/PSYS32",       "/system/psys32" },
        { "/USER",                "/user" },
        { "/user/HOME",           "/user/home" },
        { "/SHELL.BIN",           "/shell.bin" },
        { "/system/bin/LIMASM.EXC", "/system/bin/limasm.exc" },
        { "/system/KEY.CFG",      "/system/key.cfg" },
        { "/user/home/SYSCALL.ASM", "/user/home/syscall.asm" },
        { "/user/home/T.EXC",     "/user/home/t.exc" },
        { "/user/home/USER.CFG",  "/user/home/user.cfg" },
        { "/user/home/SYSCALL.LOG", "/user/home/syscall.log" },
    };
    u32 i;

    for (i = 0; i < sizeof(ren) / sizeof(ren[0]); i++)
        pfs_rename(ren[i][0], ren[i][1]);
}

static void build_tree(void)
{
    fix_names();

    pfs_mkdir("/system");
    pfs_mkdir("/system/bin");
    pfs_mkdir("/system/psys32");
    pfs_mkdir("/user");
    pfs_mkdir("/user/home");
    pfs_mkdir("/user/setting");

    /* 配置不再放家目录：把旧盘的账号文件迁到 user/setting */
    pfs_rename("/user/home/user.cfg", "/user/setting/user.cfg");
}

/* ---------------------------------------------------------------------
 * system installer
 *
 * The build script writes a linear payload into the first 64 MB of the hard
 * disk - the "install area" that the FAT volume (mounted at 64 MB) never
 * touches:
 *
 *   LBA 0      header  : "PINS", version, entry count, toc_lba, total
 *   LBA 1..    TOC     : 8 entries of 64 bytes per sector
 *   LBA n..    file data, each entry points at its own first sector
 *
 * On boot the kernel copies every payload file into the FAT volume, drops a
 * "/system/installed" marker and then wipes the install area so the staging
 * block is released again.  A disk whose first sector does not carry the
 * "PINS" magic is considered already installed and is left untouched.
 * ------------------------------------------------------------------- */
#define INST_ENT_PER_SEC 8
#define INST_MAX_ENT     128

static u8 inst_sec[512];
static u8 inst_dat[512];

static u32 inst_rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void inst_zero(u32 lba, u32 n)
{
    u32 i;

    memset(inst_dat, 0, sizeof(inst_dat));
    for (i = 0; i < n; i++)
        hdk_diskWriteSector(lba + i, inst_dat);
}

static void install_system(void)
{
    u32 count, toc, total, i, files = 0;
    char line[96];

    hdk_diskReadSector(0, inst_sec);

    if (inst_sec[0] != 'P' || inst_sec[1] != 'I' ||
        inst_sec[2] != 'N' || inst_sec[3] != 'S')
        return;

    count = inst_rd32(inst_sec + 8);
    toc   = inst_rd32(inst_sec + 12);
    total = inst_rd32(inst_sec + 16);

    if (!count || count > INST_MAX_ENT || toc < 1) {
        ioprintln("install: bad payload header");
        return;
    }

    for (i = 0; i < count; i++) {
        u8  ent[64];
        char *name;
        u32 fsec, fsize, flags, left, lba;

        hdk_diskReadSector(toc + i / INST_ENT_PER_SEC, inst_sec);
        memcpy(ent, inst_sec + (i % INST_ENT_PER_SEC) * 64, 64);
        ent[47] = 0;

        name  = (char *)ent;
        fsec  = inst_rd32(ent + 48);
        fsize = inst_rd32(ent + 52);
        flags = inst_rd32(ent + 56);

        if (!name[0])
            continue;

        if (flags & 1u) {
            pfs_mkdir(name);
            continue;
        }

        if (pfs_open_w(name) != 0) {
            ioprintln("install: cannot create");
            continue;
        }

        left = fsize;
        lba  = fsec;
        while (left) {
            u32 n = (left > sizeof(inst_dat)) ? sizeof(inst_dat) : left;

            hdk_diskReadSector(lba, inst_dat);
            if (pfs_put(inst_dat, n) != 0) {
                ioprintln("install: write error");
                break;
            }
            lba++;
            left -= n;
        }
        pfs_close_w();
        files++;
    }

    pfs_crt("/system/installed");

    if (total)
        inst_zero(0, total);

    sprintf(line, "install: %u files -> /system, %u KB released",
            files, (total * 512u) >> 10);
    ioprintln(line);
}

static void init_storage(void)
{
    char line[96];

    pfs_set_user(USER_ROOT);

    if (pfs_mount() != 0) {
        syserror("disk not mountable");
        hltsleep();
    }

    if (user_mem_bytes() < SYS_USER_MEM_MIN) {
        syserror("user memory below 64MB");
        hltsleep();
    }

    if (e820_disk_sectors() * 512u < SYS_DISK_MIN) {
        syserror("disk below 1MB");
        hltsleep();
    }

    build_tree();
    install_system();

    /* truncate the runtime log so every boot starts with a clean file */
    klog_init();

    sprintf(line, "disk: %u MB, tree /system /user ready",
            (e820_disk_sectors() >> 11));
    ioprintln(line);

    pfs_set_user(USER_NORMAL);
}

static void init_shell(void)
{
    char line[96];
    int n;
    u32 blen;

    shell_proc = proc_create("shell");
    if (!shell_proc) {
        syserror("cannot create shell process");
        hltsleep();
    }

    n = proc_load(shell_proc, SHELL_FILE);
    if (n <= 0) {
        syserror("cannot load shell.bin");
        hltsleep();
    }

    blen = 0;
    while (blen < BOOTBUF_SIZE && boot_buf[blen])
        blen++;
    if (proc_copy_in(shell_proc, BOOT_ARG_VA, boot_buf, blen + 1) < 0) {
        syserror("cannot pass boot buffer to shell");
        hltsleep();
    }

    shell_proc->arg = BOOT_ARG_VA;
    proc_boot(shell_proc);

    sprintf(line, "shell: %s %d bytes -> 0x%x, cr3=0x%x",
            SHELL_FILE, n, (u32)USER_BASE, shell_proc->cr3);
    ioprintln(line);
}

static void start_shell(void)
{
    proc_start();
    hltsleep();
}

#endif
