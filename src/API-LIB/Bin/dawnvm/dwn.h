/*
 * dwn.h - DawnByteCode (.dwn) 容器格式 + DawnVM 指令编码定义
 *
 * 这是 DawnByteCode 规则意见稿 v0.3 的落地实现所用的共享定义。
 * 纯头文件、无外部依赖，主机测试与 PoserOS 目标编译都能用。
 *
 * 寄存器：16 个物理寄存器 R0-R15，均为 32 位。
 *   - 视图名 E/X/H/L 只是同一寄存器的不同位段（类似 eax/ax/ah/al）：
 *       E = bit31..0    X = bit15..0    L = bit7..0    H = bit15..8
 *   - 类型 I/F/P/V 不参与编码，由指令本身决定。
 *
 * 编码：OP(1) + MOD(1) + [R2(1)] + [IMM/DISP]
 *   MOD = dst[7:4] | size[3:2] | kind[1:0]      (kind != 3 时)
 *   MOD = cc [7:4] | size[3:2] | kind=3[1:0]    (分支/特殊指令)
 *
 *   kind=0 reg,reg,reg : R2 = a[7:4] | b[3:0]          长度 3
 *   kind=1 reg,imm     : 后跟 imm(size 字节)            长度 2+size
 *   kind=2 reg,[base+disp] : R2 = base[7:4]|index[3:0]  长度 3+4
 *                            base=0xF 表示绝对地址；index=0xF 表示无变址
 *   kind=3 特殊         : 分支 = OP+MOD+rel32 (6 字节)
 *                         SVC  = OP+MOD+id8   (3 字节)
 *
 * 短指令 0x80-0xBF：固定 2 字节 [OP][dst:src]
 * 前缀 0xF0：后续 imm/disp 按 8 字节(EB) 读
 * 前缀 0xFF：转义，后跟 1 字节并入操作码
 */

#ifndef DWN_H
#define DWN_H

/* ---- 基础类型（自带，避免依赖 libc / Pstd） ---- */
#if !defined(DWN_TYPES_DEFINED)
#define DWN_TYPES_DEFINED
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef unsigned long long u64;
typedef signed long long   i64;
#endif

/* ---- .dwn 容器格式 ---- */
#define DWN_MAGIC0 'D'
#define DWN_MAGIC1 'W'
#define DWN_MAGIC2 'N'
#define DWN_MAGIC3 0x00
#define DWN_VERSION   1u
#define DWN_HDR_SIZE  32u

#define DWN_NREG      16
#define DWN_MEM_SIZE  (64u * 1024u)    /* v1 扁平内存 64KB */

#define DWN_SP        15               /* R15 = 栈指针 */
#define DWN_FP        14               /* R14 = 帧指针 */

typedef struct {
    u8  magic[4];
    u16 version;
    u16 hdr_size;
    u32 entry;        /* 入口 VA（.text 第一条指令） */
    u32 image_size;   /* .text + .data 总字节数 */
    u32 data_base;    /* .data 的 VA 基址 */
    u32 bss_size;     /* 未初始化数据大小 */
    u32 flags;
    u32 checksum;     /* FNV-1a，校验时本字段按 0 计 */
} __attribute__((packed)) dwn_header_t;

/* ---- MOD 位域 ---- */
#define DWN_SIZE_OB 0    /* 1 字节 */
#define DWN_SIZE_TB 1    /* 2 字节 */
#define DWN_SIZE_FB 2    /* 4 字节 */
#define DWN_SIZE_EB 3    /* 8 字节 */

#define DWN_KIND_RRR  0
#define DWN_KIND_IMM  1
#define DWN_KIND_MEM  2
#define DWN_KIND_SPEC 3

#define DWN_MK_MOD(dst, size, kind) \
    ((((u32)(dst) & 0xF) << 4) | (((u32)(size) & 0x3) << 2) | ((u32)(kind) & 0x3))
#define DWN_MK_MOD_CC(cc)           DWN_MK_MOD((cc), 0, DWN_KIND_SPEC)
#define DWN_MOD_DST(m)   (((m) >> 4) & 0xF)
#define DWN_MOD_SIZE(m)  (((m) >> 2) & 0x3)
#define DWN_MOD_KIND(m)  ((m) & 0x3)
#define DWN_MOD_CC(m)    (((m) >> 4) & 0xF)

#define DWN_SIZE_BYTES(s) (1u << (s))   /* 0->1,1->2,2->4,3->8 */

/* ---- 操作码 ---- */
enum {
    /* 0x00-0x0F 杂项 */
    OP_NOP   = 0x00,
    OP_HLT   = 0x01,
    OP_SVC   = 0x02,     /* intervm：系统/宿主调用，kind=3 + id8 */

    /* 0x10-0x1F 传送 */
    OP_LD    = 0x10,     /* dst <- [base+disp] */
    OP_ST    = 0x11,     /* [base+disp] <- dst */
    OP_LDI   = 0x12,     /* dst <- imm */
    OP_MOV   = 0x13,     /* dst <- a */

    /* 0x20-0x3F 整数 ALU */
    OP_ADD   = 0x20,
    OP_ADC   = 0x21,
    OP_SUB   = 0x22,
    OP_SBB   = 0x23,
    OP_AND   = 0x24,
    OP_OR    = 0x25,
    OP_XOR   = 0x26,
    OP_CMP   = 0x27,
    OP_TEST  = 0x28,
    OP_SHL   = 0x29,
    OP_SHR   = 0x2A,
    OP_SAR   = 0x2B,
    OP_ROL   = 0x2C,
    OP_ROR   = 0x2D,
    OP_NOT   = 0x2E,
    OP_NEG   = 0x2F,
    OP_INC   = 0x30,
    OP_DEC   = 0x31,
    OP_MUL   = 0x32,
    OP_MULU  = 0x33,
    OP_DIV   = 0x34,
    OP_DIVU  = 0x35,
    OP_REM   = 0x36,

    /* 0x40-0x4F 浮点 ALU（v1 只做 float32，寄存器存位型） */
    OP_FADD  = 0x40,
    OP_FSUB  = 0x41,
    OP_FMUL  = 0x42,
    OP_FDIV  = 0x43,
    OP_FNEG  = 0x44,     /* 翻符号位 */
    OP_FABS  = 0x45,     /* 清符号位 */
    OP_FCMP  = 0x46,     /* dst <- -1/0/1，并置 Z/C/S/O */

    /* 0x50-0x5F 控制流 */
    OP_JMP   = 0x50,     /* = Jcc with cc=0 */
    OP_JCC   = 0x51,
    OP_CALL  = 0x52,
    OP_RET   = 0x53,

    /* 0x70-0x7F 栈/内存 */
    OP_PUSH  = 0x70,
    OP_POP   = 0x71,

    /* 0x80-0xBF 2 字节短指令 */
    OP_S_INC = 0x80,
    OP_S_DEC = 0x81,
    OP_S_NOT = 0x82,
    OP_S_NEG = 0x83,
    OP_S_PUSH= 0x84,
    OP_S_POP = 0x85,
    OP_S_MOV = 0x86,
    OP_S_RET = 0x87,

    /* 前缀 */
    OP_PFX_EB = 0xF0,
    OP_ESC    = 0xFF
};

/* ---- 条件码（分支用 4 bit） ---- */
enum {
    CC_AL = 0, CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE,
    CC_LTU, CC_LEU, CC_GTU, CC_GEU,
    CC_C, CC_NC, CC_S, CC_NS, CC_O
};

/* ---- 系统调用（intervm / SVC） ----
 *
 * 调用约定：R0 = arg0，R1 = arg1；返回值写回 R0（无返回值的调用不改 R0）。
 * 也就是 EI0 / EI1 会被系统调用当成参数/返回值寄存器，写代码时注意别跟
 * 自己的数据撞车 —— 参照 x86 的 eax/ebx 易失约定。
 *
 * dst / buf / str 这类地址参数都是 DawnVM 扁平内存里的地址（变量或标签）。
 */
enum {
    DWN_SVC_PUTC      = 1,   /* (char)                          -> -      输出一个字符 */
    DWN_SVC_PRINT     = 2,   /* (str)                           -> -      输出 NUL 结尾字符串 */
    DWN_SVC_READLINE  = 3,   /* (buf, size)                     -> len    读一行，回显 */
    DWN_SVC_READLINES = 4,   /* (buf, size)                     -> len    读一行，不回显 */
    DWN_SVC_PRINT_I32 = 5,   /* (i32)                           -> -      以十进制打印有符号整数 */
    DWN_SVC_PRINT_U32 = 6,   /* (u32)                           -> -      以十进制打印无符号整数 */
    DWN_SVC_PRINT_HEX = 7,   /* (u32)                           -> -      以十六进制打印 */
    DWN_SVC_PRINT_F32 = 8,   /* (f32 bits)                      -> -      以十进制打印浮点（6 位小数） */
    DWN_SVC_GETKEY    = 9,   /* ()                              -> key    读一个键，无回显 */
    DWN_SVC_STRLEN    = 10,  /* (str)                           -> len    字符串长度 */
    DWN_SVC_STRCMP    = 11,  /* (a, b)                          -> -1/0/1 字符串比较 */
    DWN_SVC_STRCPY    = 12,  /* (dst, src)                      -> dst    字符串复制（含 NUL） */
    DWN_SVC_ATOI      = 13,  /* (str)                           -> i32    字符串转整数 */
    DWN_SVC_ITOA      = 14,  /* (buf, i32)                      -> len    有符号整数转字符串（写 NUL） */
    DWN_SVC_UTOA      = 15,  /* (buf, u32)                      -> len    无符号整数转字符串（写 NUL） */
    DWN_SVC_FTOA      = 16,  /* (buf, f32 bits)                 -> len    浮点转字符串（6 位小数，写 NUL） */
    DWN_SVC_EXIT      = 17,  /* (code)                          -> -      结束程序 */
    DWN_SVC_SETMODE   = 18,  /* (mode)                          -> -      切换运行模式（0 普通 / 1 包/UI） */
    DWN_SVC_TOUPPER   = 19,  /* (char)                          -> char   单字符转大写 */
    DWN_SVC_TOLOWER   = 20,  /* (char)                          -> char   单字符转小写 */
    DWN_SVC_STRUPR    = 21,  /* (str)                           -> str    字符串原地转大写 */
    DWN_SVC_STRLWR    = 22,  /* (str)                           -> str    字符串原地转小写 */
    DWN_SVC_SORT_I32  = 23,  /* (buf, count)                    -> -      有符号整数数组升序排序（原地） */
    DWN_SVC_SORT_I32D = 24,  /* (buf, count)                    -> -      有符号整数数组降序排序（原地） */
    DWN_SVC_SORT_F32  = 25,  /* (buf, count)                    -> -      float32 数组升序排序（原地） */
    DWN_SVC_SORT_F32D = 26,  /* (buf, count)                    -> -      float32 数组降序排序（原地） */
    DWN_SVC_MEMZERO   = 27,  /* (ptr, count)                    -> -      把 count 字节清零 */
    DWN_SVC_ATOF      = 28,  /* (str)                           -> f32 bits 字符串转浮点 */
    DWN_SVC_PRINTC    = 29,  /* (str, attr)                     -> -      带颜色输出，attr = fg | bg<<4（仅包模式） */
    DWN_SVC_GOTOXY    = 30,  /* (row, col)                      -> -      移动文本光标（仅包模式） */
    DWN_SVC_CLEAR     = 31,  /* ()                              -> -      清屏（仅包模式） */
    DWN_SVC_GETMODE   = 32   /* ()                              -> mode   读取当前运行模式 */
};

/* 运行模式：DWN_SVC_SETMODE 的 mode 取值 */
#define DWN_MODE_NORMAL  0
#define DWN_MODE_PACKAGE 1   /* 包模式：允许 UI / 扩展功能 */

#endif /* DWN_H */
