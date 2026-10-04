/*
 * dwn_svc_test.c - DawnVM 系统调用（intervm / SVC）宿主单元测试
 *
 * 直接把 dawnvm.c 编进来，构造 VM 内存 / 寄存器后调用 do_svc，
 * 校验字符串/数字转换、大小写、排序、IO 等系统调用的行为。
 *
 *   clang -O1 -I. dwn_svc_test.c -o dwn_svc_test
 *   ./dwn_svc_test
 */

#include <stdio.h>
#include <string.h>

/* ---- 输出捕获（dwn_host_putc 是强符号，dawnvm.c 只 extern 声明） ---- */
static char g_out[512];
static int  g_outn;

void dwn_host_putc(int c)
{
    if (g_outn < (int)sizeof(g_out) - 1)
        g_out[g_outn++] = (char)c;
    g_out[g_outn] = 0;
}

#include "API-LIB/Bin/dawnvm/dwn.h"
#include "API-LIB/Bin/dawnvm/dawnvm.c"

static int g_ok, g_bad;

static void chk(int cond, const char *what)
{
    if (cond)
        g_ok++;
    else {
        g_bad++;
        printf("FAIL: %s\n", what);
    }
}

static u32 svc(u32 id, u32 a0, u32 a1)
{
    g_vm.r[0] = a0;
    g_vm.r[1] = a1;
    do_svc(id);
    return g_vm.r[0];
}

static void wstr(u32 a, const char *s)
{
    while (*s)
        g_vm.mem[a++] = (u8)*s++;
    g_vm.mem[a] = 0;
}

static int streq(u32 a, const char *w)
{
    for (;;) {
        if (g_vm.mem[a] != (u8)*w)
            return 0;
        if (!*w)
            return 1;
        a++;
        w++;
    }
}

static void wu32(u32 a, u32 v)
{
    g_vm.mem[a]     = (u8)v;
    g_vm.mem[a + 1] = (u8)(v >> 8);
    g_vm.mem[a + 2] = (u8)(v >> 16);
    g_vm.mem[a + 3] = (u8)(v >> 24);
}

static u32 ru32(u32 a)
{
    return (u32)g_vm.mem[a] | ((u32)g_vm.mem[a + 1] << 8) |
           ((u32)g_vm.mem[a + 2] << 16) | ((u32)g_vm.mem[a + 3] << 24);
}

static void outc(const char *s, const char *what)
{
    chk(strcmp(g_out, s) == 0, what);
    g_outn = 0;
    g_out[0] = 0;
}

/* ---- 测试项 ---- */

static void test_itoa(void)
{
    chk(svc(DWN_SVC_ITOA, 0x100, (u32)-12345) == 6, "itoa len");
    chk(streq(0x100, "-12345"), "itoa -12345");
    chk(svc(DWN_SVC_ITOA, 0x100, 0) == 1, "itoa zero len");
    chk(streq(0x100, "0"), "itoa 0");
    chk(svc(DWN_SVC_ITOA, 0x100, 2147483647u) == 10, "itoa INT_MAX len");
    chk(streq(0x100, "2147483647"), "itoa INT_MAX");
    chk(svc(DWN_SVC_ITOA, 0x100, 0x80000000u) == 11, "itoa INT_MIN len");
    chk(streq(0x100, "-2147483648"), "itoa INT_MIN");
}

static void test_utoa(void)
{
    chk(svc(DWN_SVC_UTOA, 0x100, 4294967295u) == 10, "utoa UINT_MAX len");
    chk(streq(0x100, "4294967295"), "utoa UINT_MAX");
}

static void test_atoi(void)
{
    wstr(0x200, "  -42xyz");
    chk((i32)svc(DWN_SVC_ATOI, 0x200, 0) == -42, "atoi -42");
    wstr(0x200, "+1234");
    chk((i32)svc(DWN_SVC_ATOI, 0x200, 0) == 1234, "atoi +1234");
    wstr(0x200, "nothing");
    chk((i32)svc(DWN_SVC_ATOI, 0x200, 0) == 0, "atoi junk -> 0");
}

static void test_case(void)
{
    wstr(0x300, "Hello, World! 123");
    svc(DWN_SVC_STRUPR, 0x300, 0);
    chk(streq(0x300, "HELLO, WORLD! 123"), "strupr");
    svc(DWN_SVC_STRLWR, 0x300, 0);
    chk(streq(0x300, "hello, world! 123"), "strlwr");
    chk(svc(DWN_SVC_TOUPPER, 'a', 0) == 'A', "toupper a");
    chk(svc(DWN_SVC_TOLOWER, 'Z', 0) == 'z', "tolower Z");
    chk(svc(DWN_SVC_TOUPPER, '5', 0) == '5', "toupper digit");
}

static void test_str(void)
{
    wstr(0x300, "hello");
    chk(svc(DWN_SVC_STRLEN, 0x300, 0) == 5, "strlen");

    wstr(0x310, "abc");
    wstr(0x320, "abd");
    chk((i32)svc(DWN_SVC_STRCMP, 0x310, 0x320) == -1, "strcmp lt");
    chk((i32)svc(DWN_SVC_STRCMP, 0x320, 0x310) == 1, "strcmp gt");
    chk((i32)svc(DWN_SVC_STRCMP, 0x310, 0x310) == 0, "strcmp eq");

    svc(DWN_SVC_STRCPY, 0x400, 0x310);
    chk(streq(0x400, "abc"), "strcpy");
}

static void test_sort_i32(void)
{
    u32 base = 0x500;
    u32 vals[6] = { 5u, (u32)-3, 9u, 0u, (u32)-3, 7u };
    int i;
    for (i = 0; i < 6; i++)
        wu32(base + (u32)i * 4u, vals[i]);

    svc(DWN_SVC_SORT_I32, base, 6);
    chk((i32)ru32(base + 0) == -3 && (i32)ru32(base + 4) == -3 &&
        (i32)ru32(base + 8) == 0 && (i32)ru32(base + 12) == 5 &&
        (i32)ru32(base + 16) == 7 && (i32)ru32(base + 20) == 9, "sort i32 asc");

    svc(DWN_SVC_SORT_I32D, base, 6);
    chk((i32)ru32(base + 0) == 9 && (i32)ru32(base + 20) == -3, "sort i32 desc");
}

static void test_sort_f32(void)
{
    u32 base = 0x600;
    wu32(base + 0,  0x40400000u);   /* 3.0 */
    wu32(base + 4,  0x3FC00000u);   /* 1.5 */
    wu32(base + 8,  0xC0000000u);   /* -2.0 */
    wu32(base + 12, 0x00000000u);   /* 0.0 */

    svc(DWN_SVC_SORT_F32, base, 4);
    chk(ru32(base + 0) == 0xC0000000u && ru32(base + 4) == 0x00000000u &&
        ru32(base + 8) == 0x3FC00000u && ru32(base + 12) == 0x40400000u, "sort f32 asc");
}

static void test_ftoa(void)
{
    chk(svc(DWN_SVC_FTOA, 0x700, 0x40600000u) == 8 && streq(0x700, "3.500000"), "ftoa 3.5");
    chk(streq(0x700, "3.500000"), "ftoa 3.5 str");
    svc(DWN_SVC_FTOA, 0x700, 0xC1440000u);
    chk(streq(0x700, "-12.250000"), "ftoa -12.25");
    svc(DWN_SVC_FTOA, 0x700, 0x42C80000u);
    chk(streq(0x700, "100.000000"), "ftoa 100");
    svc(DWN_SVC_FTOA, 0x700, 0x3EAAAAABu);
    chk(streq(0x700, "0.333333"), "ftoa 1/3");
    svc(DWN_SVC_FTOA, 0x700, 0x00000000u);
    chk(streq(0x700, "0.000000"), "ftoa zero");
    svc(DWN_SVC_FTOA, 0x700, 0x7F800000u);
    chk(streq(0x700, "inf"), "ftoa inf");
}

static void test_atof(void)
{
    wstr(0x200, "3.5");
    chk(svc(DWN_SVC_ATOF, 0x200, 0) == 0x40600000u, "atof 3.5");
    wstr(0x200, "-12.25");
    chk(svc(DWN_SVC_ATOF, 0x200, 0) == 0xC1440000u, "atof -12.25");
    wstr(0x200, "1000");
    chk(svc(DWN_SVC_ATOF, 0x200, 0) == 0x447A0000u, "atof 1000");
    wstr(0x200, "0.1");
    chk(svc(DWN_SVC_ATOF, 0x200, 0) == 0x3DCCCCCDu, "atof 0.1");
    wstr(0x200, "1e3");
    chk(svc(DWN_SVC_ATOF, 0x200, 0) == 0x447A0000u, "atof 1e3");
}

static void test_print(void)
{
    g_outn = 0; g_out[0] = 0;
    svc(DWN_SVC_PUTC, 'X', 0);
    outc("X", "putc");

    svc(DWN_SVC_PRINT_I32, (u32)-7, 0);
    outc("-7", "print_i32");

    svc(DWN_SVC_PRINT_U32, 4000000000u, 0);
    outc("4000000000", "print_u32");

    svc(DWN_SVC_PRINT_HEX, 0xDEADBEEFu, 0);
    outc("deadbeef", "print_hex");

    svc(DWN_SVC_PRINT_F32, 0x40600000u, 0);
    outc("3.500000", "print_f32");

    wstr(0x200, "hi there");
    svc(DWN_SVC_PRINT, 0x200, 0);
    outc("hi there", "print str");
}

static void test_misc(void)
{
    int i;
    for (i = 0; i < 8; i++)
        g_vm.mem[0x800 + i] = 0xAA;
    svc(DWN_SVC_MEMZERO, 0x800, 8);
    for (i = 0; i < 8; i++)
        if (g_vm.mem[0x800 + i]) { chk(0, "memzero"); return; }
    chk(1, "memzero");

    g_vm.mode = DWN_MODE_NORMAL;
    svc(DWN_SVC_SETMODE, DWN_MODE_PACKAGE, 0);
    chk(g_vm.mode == DWN_MODE_PACKAGE, "setmode package");
    chk(svc(DWN_SVC_GETMODE, 0, 0) == DWN_MODE_PACKAGE, "getmode package");

    /* UI 调用：弱默认钩子下 PRINTC 退化成普通输出；GOTOXY/CLEAR 无副作用 */
    wstr(0x200, "ui");
    g_outn = 0; g_out[0] = 0;
    svc(DWN_SVC_PRINTC, 0x200, 0x1E);
    outc("ui", "printc package");
    svc(DWN_SVC_GOTOXY, 3, 5);
    svc(DWN_SVC_CLEAR, 0, 0);
    chk(1, "gotoxy/clear noop");

    /* 非包模式：PRINTC 降级为普通输出，GOTOXY/CLEAR 忽略 */
    svc(DWN_SVC_SETMODE, DWN_MODE_NORMAL, 0);
    chk(svc(DWN_SVC_GETMODE, 0, 0) == DWN_MODE_NORMAL, "getmode normal");
    g_outn = 0; g_out[0] = 0;
    svc(DWN_SVC_PRINTC, 0x200, 0x1E);
    outc("ui", "printc normal fallback");

    /* 弱默认钩子：没有输入源时 readline 安全返回 0 */
    chk(svc(DWN_SVC_READLINE, 0x900, 16) == 0, "readline default");
    chk(svc(DWN_SVC_GETKEY, 0, 0) == (u32)-1, "getkey default");
}

int main(void)
{
    memset(g_vm.mem, 0, sizeof(g_vm.mem));
    test_itoa();
    test_utoa();
    test_atoi();
    test_case();
    test_str();
    test_sort_i32();
    test_sort_f32();
    test_ftoa();
    test_atof();
    test_print();
    test_misc();
    printf("dwn_svc_test: ok=%d bad=%d\n", g_ok, g_bad);
    return g_bad ? 1 : 0;
}
