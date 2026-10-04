#ifndef PSTD_KAPI_H
#define PSTD_KAPI_H

#include "pstd_int.h"

#define SYS_PUTC        1
#define SYS_IOPRINT     2
#define SYS_IOPRINTLN   3
#define SYS_IOINPUT     4
#define SYS_IOINPUTS    5
#define SYS_FS_CRT      6
#define SYS_FS_WRITE    7
#define SYS_FS_READ     8
#define SYS_FS_REMOVE   9
#define SYS_SET_USER    10
#define SYS_GET_USER    11
#define SYS_HALT        12
#define SYS_REBOOT      13
#define SYS_IOPRINTC    14
#define SYS_EXEC        15
#define SYS_WAIT        16
#define SYS_EXIT        17
#define SYS_TIME        18
#define SYS_CLEAR       19
#define SYS_FS_MKDIR    20
#define SYS_FS_LIST     21
#define SYS_FS_RMDIR    22
#define SYS_FS_COPY     23
#define SYS_FS_RENAME   24
#define SYS_GETKEY      25
#define SYS_GOTOXY      26

static inline int sys_call3(u32 n, u32 a, u32 b, u32 c)
{
    int r;

    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return r;
}

static inline void sys_putc(char c)
{
    sys_call3(SYS_PUTC, (u32)(unsigned char)c, 0, 0);
}

static inline void sys_ioprint(const char *s)
{
    sys_call3(SYS_IOPRINT, (u32)s, 0, 0);
}

static inline void sys_ioprintln(const char *s)
{
    sys_call3(SYS_IOPRINTLN, (u32)s, 0, 0);
}

static inline void sys_ioprintc(const char *s, unsigned int fg, unsigned int bg)
{
    sys_call3(SYS_IOPRINTC, (u32)s, fg, bg);
}

static inline int sys_ioinput(char *buf, unsigned int size)
{
    return sys_call3(SYS_IOINPUT, (u32)buf, size, 0);
}

static inline int sys_ioinputs(char *buf, unsigned int size)
{
    return sys_call3(SYS_IOINPUTS, (u32)buf, size, 0);
}

static inline int sys_fs_crt(const char *path)
{
    return sys_call3(SYS_FS_CRT, (u32)path, 0, 0);
}

static inline int sys_fs_write(const char *path, const void *buf, unsigned int len)
{
    return sys_call3(SYS_FS_WRITE, (u32)path, (u32)buf, len);
}

static inline int sys_fs_read(const char *path, void *buf, unsigned int len)
{
    return sys_call3(SYS_FS_READ, (u32)path, (u32)buf, len);
}

static inline int sys_fs_remove(const char *path)
{
    return sys_call3(SYS_FS_REMOVE, (u32)path, 0, 0);
}

static inline int sys_fs_mkdir(const char *path)
{
    return sys_call3(SYS_FS_MKDIR, (u32)path, 0, 0);
}

static inline int sys_fs_rmdir(const char *path)
{
    return sys_call3(SYS_FS_RMDIR, (u32)path, 0, 0);
}

static inline int sys_fs_list(const char *path, char *buf, unsigned int size)
{
    return sys_call3(SYS_FS_LIST, (u32)path, (u32)buf, size);
}

static inline int sys_fs_copy(const char *src, const char *dst)
{
    return sys_call3(SYS_FS_COPY, (u32)src, (u32)dst, 0);
}

static inline int sys_fs_rename(const char *src, const char *dst)
{
    return sys_call3(SYS_FS_RENAME, (u32)src, (u32)dst, 0);
}

static inline void sys_set_user(int user)
{
    sys_call3(SYS_SET_USER, (u32)user, 0, 0);
}

static inline int sys_get_user(void)
{
    return sys_call3(SYS_GET_USER, 0, 0, 0);
}

static inline void sys_halt(void)
{
    sys_call3(SYS_HALT, 0, 0, 0);
}

static inline void sys_reboot(void)
{
    sys_call3(SYS_REBOOT, 0, 0, 0);
}

static inline int sys_exec(const char *path, const char *args, unsigned int ring0)
{
    return sys_call3(SYS_EXEC, (u32)path, (u32)args, ring0);
}

static inline int sys_wait(int pid)
{
    return sys_call3(SYS_WAIT, (u32)pid, 0, 0);
}

static inline void sys_exit(int code)
{
    sys_call3(SYS_EXIT, (u32)code, 0, 0);
}

static inline unsigned int sys_time(void)
{
    return (unsigned int)sys_call3(SYS_TIME, 0, 0, 0);
}

static inline void sys_clear(void)
{
    sys_call3(SYS_CLEAR, 0, 0, 0);
}

/* read one raw key; control keys arrive as 0x01..0x1A (Ctrl+A..Z) */
static inline int sys_getkey(void)
{
    return sys_call3(SYS_GETKEY, 0, 0, 0);
}

/* 把输出光标移到 (row, col)，用于局部刷新（0-based，屏幕 80x25） */
static inline void sys_gotoxy(int row, int col)
{
    sys_call3(SYS_GOTOXY, (u32)row, (u32)col, 0);
}

#endif
