#ifndef PSTD_DWN_H
#define PSTD_DWN_H

#include "pstd_int.h"

/*
 * DawnByteCode (.dwn) + DawnVM 的对外接口。
 *
 * 内核 / 安装包只要链接 dawnvm.c + dawnbc.c，就可以：
 *   - 用 dawnbc_loadscript(path) 直接加载并运行一个 .dwn 脚本；
 *   - 或自己持有镜像 buf，用 dwn_run(buf, len) 运行。
 * 两者都返回程序的退出码（>=0），或下面的负错误码。
 *
 * 输出钩子 dwn_host_putc 由 dawnbc.c 提供（走 SYS_PUTC）；
 * 若平台想要别的输出方式，自己实现该钩子、不链接 dawnbc.c 即可。
 */

#define DWN_ERR_TOO_SHORT (-1)   /* 镜像比头还短 */
#define DWN_ERR_MAGIC     (-2)   /* 魔数不是 DWN\0 */
#define DWN_ERR_VERSION   (-3)   /* 版本 / 头大小不符 */
#define DWN_ERR_TRUNC     (-4)   /* image_size 与实际长度不符 */
#define DWN_ERR_TOOBIG    (-5)   /* image_size 超过 VM 内存 */
#define DWN_ERR_TIMEOUT   (-6)   /* 指令数超出上限（疑似死循环） */
#define DWN_ERR_CHECKSUM  (-7)   /* 校验和不符 */
#define DWN_ERR_OPEN      (-8)   /* 打不开 / 读不出脚本文件 */

/* 运行一个已加载到内存的 .dwn 镜像；返回退出码或负错误码 */
int dwn_run(const u8 *img, u32 len);

/* 从文件系统读入 path 并运行；返回退出码或负错误码 */
int dawnbc_loadscript(const char *path);

#endif /* PSTD_DWN_H */
