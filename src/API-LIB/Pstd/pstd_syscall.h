#ifndef PSTD_SYSCALL_H
#define PSTD_SYSCALL_H

#include "pstd_int.h"
#include "pstd_io.h"
#include "pstd_kernel.h"
#include "pstd_fs.h"
#include "pstd_klog.h"
#include "pstd_proc.h"
#include "pstd_kapi.h"

static u32 syscall_dispatch(u32 n, u32 a0, u32 a1, u32 a2)
{
    klog_syscall(n, a0, a1, a2);

    switch (n) {
    case SYS_PUTC:      putc((char)a0);                          return 0;
    case SYS_IOPRINT:   ioprint((const char *)a0);               return 0;
    case SYS_IOPRINTLN: ioprintln((const char *)a0);             return 0;
    case SYS_IOPRINTC:  ioprintc((const char *)a0, (u8)a1, (u8)a2); return 0;
    case SYS_IOINPUT:   return (u32)ioinput((char *)a0, a1);
    case SYS_IOINPUTS:  return (u32)ioinputs((char *)a0, a1);
    case SYS_FS_CRT:    return (u32)pfs_crt((const char *)a0);
    case SYS_FS_WRITE:  return (u32)pfs_write((const char *)a0, (const void *)a1, a2);
    case SYS_FS_READ:   return (u32)pfs_read((const char *)a0, (void *)a1, a2);
    case SYS_FS_REMOVE: return (u32)pfs_remove((const char *)a0);
    case SYS_SET_USER:  pfs_set_user((int)a0);                   return 0;
    case SYS_GET_USER:  return (u32)pfs_get_user();
    case SYS_HALT:      hltsleep();                              return 0;
    case SYS_REBOOT:    reboot();                                return 0;
    case SYS_EXEC:      return (u32)proc_exec((const char *)a0, (const char *)a1, a2);
    case SYS_WAIT:      return (u32)proc_wait((int)a0);
    case SYS_EXIT:      proc_exit(a0);                           return 0;
    case SYS_TIME:      return rtcDosTime();
    case SYS_CLEAR:     clearScreen();                          return 0;
    case SYS_FS_MKDIR:  return (u32)pfs_mkdir((const char *)a0);
    case SYS_FS_RMDIR:  return (u32)pfs_rmdir((const char *)a0);
    case SYS_FS_LIST:   return (u32)pfs_list((const char *)a0, (char *)a1, a2);
    case SYS_FS_COPY:   return (u32)pfs_copy((const char *)a0, (const char *)a1);
    case SYS_FS_RENAME: return (u32)pfs_rename((const char *)a0, (const char *)a1);
    case SYS_GETKEY:    return (u32)kb_getch();
    case SYS_GOTOXY:    gotoXY((int)a0, (int)a1);               return 0;
    default:            return (u32)-1;
    }
}

#endif
