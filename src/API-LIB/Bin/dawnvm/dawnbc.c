/*
 * dawnbc.c - DawnVM 的 PoserOS 宿主层
 *
 * 提供：
 *   - dwn_host_putc：VM 的输出钩子（走 SYS_PUTC）
 *   - dawnbc_loadscript(path)：读文件 + dwn_run，返回退出码或负错误码
 *
 * 安装包 / 内核直接调用 dawnbc_loadscript 即可运行 .dwn 脚本，
 * 不会退出当前进程（退出码通过返回值给出）。
 */

#include "API-LIB/Pstd/pstd_dwn.h"
#include "API-LIB/Pstd/pstd_kapi.h"

/* 容器常量（DWN_HDR_SIZE 等）来自 dwn.h；类型已由 pstd_int.h 提供 */
#define DWN_TYPES_DEFINED
#include "dwn.h"

/* 一个 .dwn 文件 = 32 字节头 + 最多 64KB 镜像 */
#define DAWNBC_MAX_FILE (DWN_HDR_SIZE + 65536u)

static u8 g_img[DAWNBC_MAX_FILE] __attribute__((section(".data"))) = {1};

void dwn_host_putc(int c)
{
    sys_putc((char)c);
}

/* DawnVM 的输入 / 模式钩子（强符号，覆盖 dawnvm.c 里的弱默认） */
int dwn_host_getkey(void)
{
    return sys_getkey();
}

int dwn_host_readline(char *buf, int size, int echo)
{
    if (size <= 0)
        return 0;
    return echo ? sys_ioinput(buf, (unsigned)size)
                : sys_ioinputs(buf, (unsigned)size);
}

/* 包模式切换钩子：目前 VM 内部记录模式，平台侧暂不需要额外动作 */
void dwn_host_setmode(int mode)
{
    (void)mode;
}

/* UI 钩子：包模式下由 VM 的 PRINTC/GOTOXY/CLEAR 调用，转发到内核文本输出 */
void dwn_host_printc(const char *s, int fg, int bg)
{
    sys_ioprintc(s, (unsigned)fg, (unsigned)bg);
}

void dwn_host_gotoxy(int row, int col)
{
    sys_gotoxy(row, col);
}

void dwn_host_clear(void)
{
    sys_clear();
}

int dawnbc_loadscript(const char *path)
{
    int n;

    if (!path || !path[0])
        return DWN_ERR_OPEN;

    n = sys_fs_read(path, g_img, DAWNBC_MAX_FILE);
    if (n <= 0)
        return DWN_ERR_OPEN;

    return dwn_run(g_img, (u32)n);
}
