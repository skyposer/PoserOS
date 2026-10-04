/*
 * rundwn - DawnByteCode (.dwn) 运行器
 *
 *   usage: rundwn <prog.dwn>
 *
 * 直接调用 DawnVM 宿主接口 dawnbc_loadscript(path)：
 *   - 成功：程序退出码打到控制台，本进程退出码 0
 *   - 失败：返回负错误码，打印错误信息，本进程退出码 1
 *
 * 和 dasm 配合：dasm 把 .dasm 编成 .dwn，rundwn 负责跑 .dwn。
 */

#include "API-LIB/Pstd/pstd_dwn.h"
#include "API-LIB/Pstd/pstd_kapi.h"
#include "API-LIB/Pstd/pstd_string.h"

/* 取字符串里的第一个空白分隔的单词（原地截断） */
static char *first_word(char **pp)
{
    char *s = *pp;
    char *b;

    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s) {
        *pp = s;
        return 0;
    }
    b = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s)
        *s++ = 0;
    *pp = s;
    return b;
}

__attribute__((section(".text._start"), used, noreturn))
void rundwn_entry(void)
{
    char *args, *p, *path;
    char b[160];
    int rc;

    __asm__ volatile("mov %%eax, %0" : "=r"(args));
    args = (char *)args;

    if (!args || !args[0]) {
        sys_ioprintln("rundwn: usage: rundwn <prog.dwn>");
        sys_exit(1);
    }

    p = args;
    path = first_word(&p);
    if (!path) {
        sys_ioprintln("rundwn: usage: rundwn <prog.dwn>");
        sys_exit(1);
    }

    rc = dawnbc_loadscript(path);

    if (rc < 0) {
        sprintf(b, "rundwn: cannot run %s (error %d)", path, rc);
        sys_ioprintln(b);
        sys_exit(1);
    }

    sprintf(b, "rundwn: %s exited with %d", path, rc);
    sys_ioprintln(b);
    sys_exit(0);

    for (;;)
        ;
}
