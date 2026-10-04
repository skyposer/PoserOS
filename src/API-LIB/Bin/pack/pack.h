/*
 * pack.h - PoserOS 安装包（.pack）容器格式 + 解析
 *
 * .pack 就是把一堆文件归档进单个文件的容器，必须包含一份 PackageMain.ini。
 * 结构（全部小端）：
 *
 *   [header 32 字节]
 *     0   u8[4]  magic       "PACK"
 *     4   u16    version     1
 *     6   u16    hdr_size    32
 *     8   u32    file_count  目录项个数
 *     12  u32    toc_off     目录表绝对偏移（=32）
 *     16  u32    data_off    数据区绝对偏移
 *     20  u32    flags       保留
 *     24  u32    total_size  整个文件字节数
 *     28  u32    checksum    FNV-1a（本字段按 0 计），覆盖整个文件
 *
 *   [TOC] file_count * 64 字节
 *     0   char name[52]  归档内路径，'/' 分隔，无前导 '/'，NUL 补齐
 *     52  u32  off       文件数据绝对偏移
 *     56  u32  size      字节数（目录为 0）
 *     60  u32  type      0=文件 1=目录
 *
 *   [data] 各文件内容连续存放
 *
 * PackageMain.ini 内容：
 *   [header]
 *   sys=PoserOS
 *   arch=x86
 *   [info]
 *   name=...
 *   version=...
 *   author=...
 *   [starting]
 *   ready=/ReadyLoader.dwn     ; 必须存在；跑起来后用 intervm 18 切到包模式
 *   type=DBC                   ; DBC（字节码）或 ExecutableFile
 *   main=/pack/启动的主文件
 *
 * 解析层不碰文件系统，纯 buf/len，方便宿主单元测试与内核复用。
 */

#ifndef DWN_PACK_H
#define DWN_PACK_H

#ifndef PACK_NO_PSTD_TYPES
#include "pstd_int.h"
#else
#ifndef PACK_TYPES_DEFINED
#define PACK_TYPES_DEFINED
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
#endif
#endif

/* ---- 容器常量 ---- */
#define PACK_MAGIC0    'P'
#define PACK_MAGIC1    'A'
#define PACK_MAGIC2    'C'
#define PACK_MAGIC3    'K'

#define PACK_VERSION   1u
#define PACK_HDR_SIZE  32u
#define PACK_ENT_SIZE  64u
#define PACK_NAME_MAX  52
#define PACK_PATH_MAX  128
#define PACK_MAX_FILES 128

#define PACK_TYPE_FILE 0u
#define PACK_TYPE_DIR  1u

/* ---- 错误码 ---- */
#define PACK_OK            0
#define PACK_ERR_MAGIC   (-1)
#define PACK_ERR_VER     (-2)
#define PACK_ERR_SIZE    (-3)
#define PACK_ERR_TOC     (-4)
#define PACK_ERR_ENT     (-5)
#define PACK_ERR_CHECK   (-6)
#define PACK_ERR_FULL    (-7)
#define PACK_ERR_NOENT   (-8)
#define PACK_ERR_TYPE    (-9)

typedef struct {
    char name[PACK_NAME_MAX];   /* 归档内路径 */
    u32  off;                   /* 数据绝对偏移 */
    u32  size;                  /* 字节数 */
    u32  type;                  /* PACK_TYPE_* */
} pack_ent_t;

typedef struct {
    const u8 *buf;              /* 整个 .pack 文件的内存镜像 */
    u32       len;
    u32       count;            /* TOC 项数 */
    u32       toc_off;
    u32       data_off;
    u32       total_size;
    pack_ent_t ent[PACK_MAX_FILES];
} pack_t;

/* ---- INI ---- */
#define PACK_INI_FILE  "PackageMain.ini"

typedef struct {
    char sys[32];
    char arch[32];
    char name[64];
    char version[32];
    char author[64];
    char ready[PACK_PATH_MAX];  /* 如 "/ReadyLoader.dwn"，空 = 普通程序无 UI */
    char type[24];              /* "DBC" / "ExecutableFile" */
    char main[PACK_PATH_MAX];   /* 如 "/pack/main.dwn" */
} pack_ini_t;

/* ---- API ---- */

/* FNV-1a；checksum 字段(28..31)按 0 计 */
u32 pack_checksum(const u8 *buf, u32 len);

/* 解析内存里的 .pack 镜像 */
int pack_parse(const u8 *buf, u32 len, pack_t *p);

/* 按归档内路径查表；name 可带/不带前导 '/'，大小写不敏感 */
const pack_ent_t *pack_find(const pack_t *p, const char *name);

/* 读出条目内容到 dst；cap 不够返回 PACK_ERR_FULL；*out 回填实际长度 */
int pack_read(const pack_t *p, const pack_ent_t *e, void *dst, u32 cap, u32 *out);

/* 解析 PackageMain.ini（text 需 NUL 结尾） */
int pack_ini_parse(const char *text, pack_ini_t *ini);

/* 规整路径：去前导 '/'，折叠 "//"、"."、".."；返回长度，溢出返回 -1 */
int pack_norm_path(const char *in, char *out, u32 cap);

#endif /* DWN_PACK_H */
