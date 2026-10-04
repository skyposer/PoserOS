/*
 * edstyle.c - EditStyleVM：编辑器语法高亮引擎的实现
 *
 * 引擎本体很小：一张“语言名 -> 词法规则”的注册表 + 统一分发入口。
 * 每个语言规则负责把一行切成完整覆盖的 token 序列（空隙为 NORMAL）。
 * 内置规则：c / cpp / java（C 系）、asm / dasm（汇编）、cfg（配置）。
 */

#include "edstyle.h"

typedef struct {
    char       name[12];
    edstyle_fn fn;
} rule_t;

static rule_t g_rules[EDSTYLE_MAX_RULES];
static int    g_nrules;

/* ---------------- 基础字符工具 ---------------- */

static char lc(char c)          { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  is_alpha(char c)    { c = lc(c); return c >= 'a' && c <= 'z'; }
static int  is_digit(char c)    { return c >= '0' && c <= '9'; }
static int  is_idc(char c)      { return is_alpha(c) || is_digit(c) || c == '_' || c == '$'; }
static int  is_space(char c)    { return c == ' ' || c == '\t'; }

static int name_eq(const char *a, const char *b)
{
    while (*a && lc(*a) == lc(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

static int word_eq(const char *s, int n, const char *w)
{
    int i = 0;

    for (; i < n && w[i]; i++)
        if (lc(s[i]) != lc(w[i]))
            return 0;
    return i == n && w[i] == 0;
}

static int in_list(const char *s, int n, const char *const *list, int cnt)
{
    int i;

    for (i = 0; i < cnt; i++)
        if (word_eq(s, n, list[i]))
            return 1;
    return 0;
}

#define LIST_N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int ch_in(const char *set, char c)
{
    while (*set)
        if (*set++ == c)
            return 1;
    return 0;
}

/* 追加一个 token（超过 max 就丢弃，不影响已覆盖部分） */
static void tok(edtok_t *o, int *n, int max, int start, int len, int style)
{
    if (*n >= max || len <= 0)
        return;
    o[*n].start = start;
    o[*n].len   = len;
    o[*n].style = style;
    (*n)++;
}

/* ---------------- 注册 / 分发 ---------------- */

void edstyle_register(const char *name, edstyle_fn fn)
{
    int i, k;

    if (!name || !name[0] || !fn)
        return;

    for (i = 0; i < g_nrules; i++) {
        if (name_eq(g_rules[i].name, name)) {
            g_rules[i].fn = fn;             /* 同名覆盖 */
            return;
        }
    }
    if (g_nrules >= EDSTYLE_MAX_RULES)
        return;

    for (k = 0; name[k] && k < 11; k++)
        g_rules[g_nrules].name[k] = name[k];
    g_rules[g_nrules].name[k] = 0;
    g_rules[g_nrules].fn = fn;
    g_nrules++;
}

static int ext_eq(const char *path, const char *ext)
{
    const char *dot = 0, *slash = 0, *p = path;
    int i = 0;

    while (*p) {
        if (*p == '/')
            slash = p;
        else if (*p == '.')
            dot = p;
        p++;
    }
    if (!dot || (slash && dot < slash))
        return 0;

    dot++;
    while (ext[i]) {
        if (lc(dot[i]) != lc(ext[i]))
            return 0;
        i++;
    }
    return dot[i] == 0;
}

const char *edstyle_lang(const char *path)
{
    if (!path)
        return "";
    if (ext_eq(path, "c") || ext_eq(path, "h"))                          return "c";
    if (ext_eq(path, "cpp") || ext_eq(path, "cc") || ext_eq(path, "cxx") ||
        ext_eq(path, "hpp") || ext_eq(path, "hxx"))                      return "cpp";
    if (ext_eq(path, "java"))                                            return "java";
    if (ext_eq(path, "asm") || ext_eq(path, "s"))                        return "asm";
    if (ext_eq(path, "dasm") || ext_eq(path, "dwn"))                     return "dasm";
    if (ext_eq(path, "cfg") || ext_eq(path, "conf") || ext_eq(path, "ini")) return "cfg";
    return "";
}

int edstyle_run(const char *lang, const char *line, int len,
                edtok_t *out, int max)
{
    int i;

    if (!lang || !lang[0] || !line || len <= 0 || max <= 0)
        return 0;

    for (i = 0; i < g_nrules; i++)
        if (name_eq(g_rules[i].name, lang))
            return g_rules[i].fn(line, len, out, max);
    return 0;
}

int edstyle_line(const char *path, const char *line, int len,
                 edtok_t *out, int max)
{
    return edstyle_run(edstyle_lang(path), line, len, out, max);
}

/* ---------------- C / C++ / Java 规则 ---------------- */

static const char *const c_kw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "sizeof", "typedef", "struct", "union",
    "enum", "static", "extern", "const", "volatile", "register", "inline",
    "class", "public", "private", "protected", "virtual", "new", "delete",
    "this", "namespace", "using", "template", "try", "catch", "throw",
    "operator", "package", "import", "extends", "implements", "interface",
    "final", "instanceof", "synchronized", "abstract", "throw", "super"
};

static const char *const c_ty[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64",
    "usize", "isize", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "boolean", "byte", "string"
};

static int lex_c(const char *l, int len, edtok_t *o, int max)
{
    int i = 0, n = 0;

    while (i < len && n < max) {
        int s = i, st = EDST_NORMAL;
        char c = l[i];

        if (c == '/' && i + 1 < len && l[i + 1] == '/') {          /* // 行注释 */
            st = EDST_COMMENT;
            i = len;
        } else if (c == '/' && i + 1 < len && l[i + 1] == '*') {   /* 块注释 */
            st = EDST_COMMENT;
            i += 2;
            while (i < len && !(l[i] == '*' && i + 1 < len && l[i + 1] == '/'))
                i++;
            if (i < len)
                i += 2;
        } else if (c == '#') {                                     /* 预处理 */
            st = EDST_PREPROC;
            i = len;
        } else if (c == '"' || c == '\'') {                        /* 字符串/字符 */
            char q = c;
            st = EDST_STRING;
            i++;
            while (i < len) {
                if (l[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (l[i] == q) { i++; break; }
                i++;
            }
        } else if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(l[i + 1]))) {
            st = EDST_NUMBER;
            i++;
            while (i < len && (is_idc(l[i]) || l[i] == '.'))
                i++;
        } else if (is_alpha(c) || c == '_') {
            int w = i, j;
            while (i < len && is_idc(l[i]))
                i++;
            if (in_list(l + w, i - w, c_kw, LIST_N(c_kw)))
                st = EDST_KEYWORD;
            else if (in_list(l + w, i - w, c_ty, LIST_N(c_ty)))
                st = EDST_TYPE;
            else {
                j = i;
                while (j < len && is_space(l[j]))
                    j++;
                if (j < len && l[j] == '(')
                    st = EDST_FUNC;
            }
        } else if (ch_in("+-*/%=<>!&|^~?:;,(){}[]", c)) {
            st = EDST_OPERATOR;
            i++;
        } else {
            i++;
        }
        tok(o, &n, max, s, i - s, st);
    }
    if (i < len)                                   /* 被 max 截断时兜底 */
        tok(o, &n, max, i, len - i, EDST_NORMAL);
    return n;
}

/* ---------------- 汇编（limasm / dasm）规则 ---------------- */

static const char *const asm_kw[] = {
    "section", "method", "function", "fn", "global", "extern", "bits", "org",
    "equ", "times", "align", "label", "db", "dw", "dd", "dq",
    "resb", "resw", "resd", "resq",
    "mov", "lea", "add", "sub", "adc", "sbb", "mul", "imul", "div", "idiv",
    "and", "or", "xor", "not", "neg", "inc", "dec", "shl", "shr", "sar", "sal",
    "rol", "ror", "cmp", "test", "jmp", "call", "ret", "push", "pop", "nop",
    "int", "int3", "hlt", "leave", "enter", "xchg", "rep", "repe", "repne",
    "cli", "sti", "cld", "std", "in", "out", "syscall",
    "lodsb", "lodsw", "lodsd", "stosb", "stosw", "stosd", "movsb", "movsw", "movsd",
    "ldi", "st", "ldb", "ldw", "ldd", "intervm", "fcmp", "fadd", "fsub", "fmul", "fdiv",
    "ji", "jie", "jne", "je", "jl", "jg", "jle", "jge", "jb", "ja", "jbe", "jae"
};

static const char *const asm_reg[] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
    "ax", "bx", "cx", "dx", "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh",
    "cs", "ds", "es", "fs", "gs", "ss",
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "pc", "sp", "flags", "ef", "ei", "ex", "hi", "lo"
};

static int is_reg_word(const char *s, int n)
{
    int i;

    if (in_list(s, n, asm_reg, LIST_N(asm_reg)))
        return 1;
    /* EAX / EI0 / EF12 / R15 这类：E|R 开头，其余字母数字，2~4 字符 */
    if (n < 2 || n > 4 || (s[0] != 'E' && s[0] != 'R'))
        return 0;
    for (i = 1; i < n; i++)
        if (!is_digit(s[i]) && !is_alpha(s[i]))
            return 0;
    return 1;
}

static int lex_asm(const char *l, int len, edtok_t *o, int max)
{
    int i = 0, n = 0, first = 1;

    while (i < len && n < max) {
        int s = i, st = EDST_NORMAL;
        char c = l[i];

        if (c == ';' || c == '#') {                    /* 注释 */
            st = (c == '#') ? EDST_PREPROC : EDST_COMMENT;
            i = len;
        } else if (c == '"') {                         /* 字符串 */
            st = EDST_STRING;
            i++;
            while (i < len) {
                if (l[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (l[i] == '"') { i++; break; }
                i++;
            }
        } else if (c == '.') {                         /* .text 之类伪指令 */
            st = EDST_PREPROC;
            i++;
            while (i < len && is_idc(l[i]))
                i++;
        } else if (c == '[' && first) {                /* [section] */
            st = EDST_SECTION;
            i++;
            while (i < len && l[i] != ']')
                i++;
            if (i < len)
                i++;
        } else if (is_digit(c) || (c == '0' && i + 1 < len && (lc(l[i+1]) == 'x'))) {
            st = EDST_NUMBER;
            i++;
            while (i < len && (is_idc(l[i]) || l[i] == '.'))
                i++;
        } else if (is_alpha(c) || c == '_') {
            int w = i, j;
            while (i < len && is_idc(l[i]))
                i++;
            j = i;
            while (j < len && is_space(l[j]))
                j++;
            if (j < len && l[j] == ':')
                st = EDST_LABEL;                       /* name: */
            else if (in_list(l + w, i - w, asm_kw, LIST_N(asm_kw)))
                st = EDST_KEYWORD;
            else if (is_reg_word(l + w, i - w))
                st = EDST_REGISTER;
        } else if (ch_in("+-*/%=<>!&|^~?:;,(){}[]", c)) {
            st = EDST_OPERATOR;
            i++;
        } else {
            i++;
        }
        if (!is_space(c))
            first = 0;
        tok(o, &n, max, s, i - s, st);
    }
    if (i < len)
        tok(o, &n, max, i, len - i, EDST_NORMAL);
    return n;
}

/* ---------------- 配置（.cfg）规则 ---------------- */

static int lex_cfg(const char *l, int len, edtok_t *o, int max)
{
    int i = 0, n = 0;

    while (i < len && n < max) {
        int s = i, st = EDST_NORMAL;
        char c = l[i];

        if (c == '#' || c == ';') {                    /* 注释 */
            st = EDST_COMMENT;
            i = len;
        } else if (c == '[') {                         /* [section] */
            st = EDST_SECTION;
            i++;
            while (i < len && l[i] != ']')
                i++;
            if (i < len)
                i++;
        } else if (c == '"') {
            st = EDST_STRING;
            i++;
            while (i < len) {
                if (l[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (l[i] == '"') { i++; break; }
                i++;
            }
        } else if (is_digit(c)) {
            st = EDST_NUMBER;
            i++;
            while (i < len && (is_idc(l[i]) || l[i] == '.'))
                i++;
        } else if (is_alpha(c) || c == '_') {
            int w = i, j;
            while (i < len && is_idc(l[i]))
                i++;
            j = i;
            while (j < len && is_space(l[j]))
                j++;
            if (j < len && l[j] == '=')
                st = EDST_KEYWORD;                     /* key = value */
            else {
                (void)w;
                st = EDST_NORMAL;
            }
        } else if (ch_in("=:;,()", c)) {
            st = EDST_OPERATOR;
            i++;
        } else {
            i++;
        }
        tok(o, &n, max, s, i - s, st);
    }
    if (i < len)
        tok(o, &n, max, i, len - i, EDST_NORMAL);
    return n;
}

/* ---------------- 初始化 ---------------- */

void edstyle_init(void)
{
    edstyle_register("c",    lex_c);
    edstyle_register("cpp",  lex_c);
    edstyle_register("java", lex_c);
    edstyle_register("asm",  lex_asm);
    edstyle_register("dasm", lex_asm);
    edstyle_register("cfg",  lex_cfg);
}
