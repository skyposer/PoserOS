#ifndef EDSTYLE_H
#define EDSTYLE_H

#include "pstd_int.h"

/*
 * EditStyleVM —— 编辑器语法高亮引擎（Powered by VM）
 *
 * 设计：把“一行文本 -> 一组带样式的片段”抽象成一台小虚拟机。
 *   - 每个语言 = 一段“词法规则”（edstyle_fn），注册进引擎；
 *   - 统一入口 edstyle_line()/edstyle_run() 负责按文件类型选中规则并执行；
 *   - 规则约定：token 完整覆盖整行 [0,len)，空隙一律按 EDST_NORMAL 处理，
 *     渲染层只要顺序拼色即可，无需自己判断语言。
 *
 * 典型用法：
 *     edstyle_init();                    // 启动时注册内置语言（只调一次）
 *     edstyle_register("mylang", lex);   // 需要时再注册 / 覆盖规则
 *
 *     edtok_t t[EDSTYLE_MAX_TOK];
 *     int n = edstyle_line(path, line, len, t, EDSTYLE_MAX_TOK);
 *     for (i = 0; i < n; i++) 用 palette[t[i].style] 输出 line[t[i].start .. +len)
 */

enum {
    EDST_NORMAL = 0,   /* 普通文本 */
    EDST_KEYWORD,      /* 关键字 */
    EDST_TYPE,         /* 类型名 */
    EDST_NUMBER,       /* 数字字面量 */
    EDST_STRING,       /* 字符串 / 字符 */
    EDST_COMMENT,      /* 注释 */
    EDST_PREPROC,      /* 预处理 / 伪指令 (.text, #include) */
    EDST_FUNC,         /* 函数名（后跟 '('） */
    EDST_OPERATOR,     /* 运算符 */
    EDST_REGISTER,     /* 寄存器 */
    EDST_LABEL,        /* 标签 name: */
    EDST_SECTION,      /* 段 / 节 [section] */
    EDST_MAX
};

typedef struct {
    int start;         /* 行内起始列 */
    int len;           /* 字节数 */
    int style;         /* EDST_* */
} edtok_t;

/* 词法规则：把 [0,len) 切成 token 写入 out，返回 token 数（<= max） */
typedef int (*edstyle_fn)(const char *line, int len, edtok_t *out, int max);

#define EDSTYLE_MAX_TOK    96
#define EDSTYLE_MAX_RULES  12

/* 注册内置语言规则；重复调用无副作用 */
void edstyle_init(void);

/* 按名字注册 / 覆盖一条高亮规则 */
void edstyle_register(const char *name, edstyle_fn fn);

/* 按路径扩展名解析语言名；无可高亮返回 "" */
const char *edstyle_lang(const char *path);

/* 统一入口：按语言名高亮一行 */
int edstyle_run(const char *lang, const char *line, int len,
                edtok_t *out, int max);

/* 统一入口：按路径高亮一行（内部解析扩展名） */
int edstyle_line(const char *path, const char *line, int len,
                 edtok_t *out, int max);

#endif
