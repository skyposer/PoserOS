/*
 * dwn_pipeline.c - dasm -> .dwn -> DawnVM 端到端宿主测试
 *
 * 目标机没有串口，OS 输出走 VGA，自动化不便；这里把 dasm.c / rundwn.c
 * 原样编到一个 32 位 freestanding 宿主程序里，用真正的文件读写把它们跑一遍，
 * 从而确定性地验证“汇编 -> 字节码 -> 虚拟机执行”整条链路。
 *
 * 编译两遍：
 *   clang ... -DHOST_DASM   ... -o dasm_host      （调用 dasm_entry）
 *   clang ... -DHOST_RUNDWN ... -o rundwn_host    （调用 rundwn_entry）
 * 入口约定与 OS 一致：参数字符串放在 EAX。
 */

/* 屏蔽 pstd_kapi.h（里面是 int 0x80 内联汇编，宿主上不能用），
 * 由本文件提供等价的 sys_* 实现。 */
#define PSTD_KAPI_H
#define DWN_TYPES_DEFINED

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long long   i64;

/* ---- i386 Linux 原始系统调用（不依赖 libc） ---- */
static int lsys(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return r;
}

#define LR_EXIT  1
#define LR_READ  3
#define LR_WRITE 4
#define LR_OPEN  5
#define LR_CLOSE 6

#define O_WRONLY_CT  (1 | 64 | 512)     /* O_WRONLY|O_CREAT|O_TRUNC */

static void out(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    lsys(LR_WRITE, 1, (int)s, n);
}

/* ---- dasm / rundwn / dawnbc 需要的 sys_* 接口 ---- */

void sys_ioprintln(const char *s) { out(s); out("\n"); }
void sys_ioprint(const char *s)   { out(s); }
void sys_putc(char c)             { lsys(LR_WRITE, 1, (int)&c, 1); }

int sys_fs_read(const char *p, void *b, unsigned int len)
{
    int fd = lsys(LR_OPEN, (int)p, 0, 0);
    int n;
    if (fd < 0)
        return -1;
    n = lsys(LR_READ, fd, (int)b, (int)len);
    lsys(LR_CLOSE, fd, 0, 0);
    return n;
}

int sys_fs_write(const char *p, const void *b, unsigned int len)
{
    int fd = lsys(LR_OPEN, (int)p, O_WRONLY_CT, 0644);
    int n;
    if (fd < 0)
        return -1;
    n = lsys(LR_WRITE, fd, (int)b, (int)len);
    lsys(LR_CLOSE, fd, 0, 0);
    return n;
}

void sys_exit(int code)
{
    lsys(LR_EXIT, code, 0, 0);
    for (;;)
        ;
}

void sys_ioprintc(const char *s, unsigned int fg, unsigned int bg)
{
    (void)fg; (void)bg;
    out(s);
}

/* dawnbc.c 的输入 / 模式钩子会用到这几个；宿主任意给个空实现 */
int sys_getkey(void) { return -1; }
int sys_ioinput(char *b, unsigned int n)  { (void)b; (void)n; return 0; }
int sys_ioinputs(char *b, unsigned int n) { (void)b; (void)n; return 0; }

/* ---- 被测程序 ---- */

#ifdef HOST_DASM
#include "API-LIB/Bin/dasm/dasm.c"
static void call_entry(char *a)
{
    __asm__ volatile("call dasm_entry" : : "a"((u32)a) : "memory");
}
#else
/* dawnbc.c 提供强符号钩子，关掉 dawnvm.c 里的弱默认，避免同 TU 重定义 */
#define DWN_NO_WEAK_HOST_HOOKS
#include "API-LIB/Bin/dawnvm/dawnvm.c"
#include "API-LIB/Bin/dawnvm/dawnbc.c"
#include "API-LIB/Bin/rundwn/rundwn.c"
static void call_entry(char *a)
{
    __asm__ volatile("call rundwn_entry" : : "a"((u32)a) : "memory");
}
#endif

static char g_args[600];

int host_main(int argc, char **argv)
{
    int i = 0, j;

    for (j = 1; j < argc; j++) {
        const char *p = argv[j];
        while (*p && i < 590)
            g_args[i++] = *p++;
        if (j + 1 < argc)
            g_args[i++] = ' ';
    }
    g_args[i] = 0;

    call_entry(g_args);
    return 0;
}

/* 进程入口：从栈里取 argc/argv 后转 host_main */
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "  mov (%esp), %eax\n"
    "  lea 4(%esp), %ecx\n"
    "  push %ecx\n"
    "  push %eax\n"
    "  call host_main\n"
    "  mov %eax, %ebx\n"
    "  mov $1, %eax\n"
    "  int $0x80\n"
);
