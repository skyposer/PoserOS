/* syscall_test.c - exercises the int 0x80 syscall output path from ring3.
 *
 * Built by b.sh into /user/home/t.exc and run from the PoserOS shell with:
 *     run /user/home/t.exc
 * It prints one line per syscall and writes a machine-checkable copy into
 * /user/home/syscall.log, then exits through SYS_EXIT.
 */
#include "API-LIB/Pstd/pstd_kapi.h"
#include "API-LIB/Pstd/pstd_string.h"

#define C_GREEN 2
#define C_BLACK 0

void syscall_test_entry(void);

__attribute__((section(".text._start"), used, noreturn))
void syscall_test_entry(void)
{
    char line[96];
    char *args;
    unsigned int t;
    int u, i, n;

    __asm__ volatile("mov %%eax, %0" : "=r"(args));

    sys_ioprintln("=== syscall output test ===");

    sys_ioprint("ioprint  : no lf here");
    sys_ioprintln(" <- lf appended");

    sys_ioprint("putc 0-9 : ");
    for (i = '0'; i <= '9'; i++)
        sys_putc((char)i);
    sys_putc('\n');

    sys_ioprintc("ioprintc : color ok\n", C_GREEN, C_BLACK);

    u = sys_get_user();
    n = sprintf(line, "get_user : %d (%s)", u, u == 0 ? "root" : "user");
    sys_ioprintln(line);

    t = sys_time();
    n = sprintf(line, "time     : 0x%x", t);
    sys_ioprintln(line);

    n = sprintf(line, "PASS ioprint+ioprintln+putc+ioprintc get_user=%d time=0x%x", u, t);
    if (sys_fs_write("/user/home/syscall.log", line, (unsigned int)n) == n)
        sys_ioprintln("log      : /user/home/syscall.log written");
    else
        sys_ioprintln("log      : write failed");

    sys_ioprintln("=== ok, exiting with code 0 ===");

    sys_exit(0);

    for (;;)
        ;
}
