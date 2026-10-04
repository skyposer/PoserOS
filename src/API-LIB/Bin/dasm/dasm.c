/*
 * dasm - DawnByteCode 汇编器（DawnVM 的 .dwn 汇编器）
 *
 *   usage: dasm <src.dasm> <out.dwn>
 *
 * 语法（依据 DawnByteCode_意见稿_v0.3.md）：
 *   寄存器视图：<宽度 E X H L><类型 V I F P><编号 0-f>
 *       例：EI0  EF3  XP1  HVa      别名：r0..rf / sp(=r15) / fp(=r14)
 *   寄存器对 a:b 表示 64 位值（低位在前、编号连续）
 *   指令：
 *       nop hlt ret
 *       ld st ldi mov
 *       add adc sub sbb and or xor cmp test shl shr sar rol ror
 *       mul mulu div divu rem not neg inc dec
 *       fadd fsub fmul fdiv fneg fabs fcmp
 *       jmp  jcc  <jcc 别名: je jne jl jle jg jge jb jbe ja jae js jns jo ...>
 *       call  push  pop  intervm(别名 svc)
 *   约定：op dst,a,b 三操作数；op dst,b 二操作数是 op dst,dst,b 的糖；
 *         not/neg/inc/dec/fneg/fabs 单操作数。
 *   立即数：0x.. / 0b.. / 十进制 / 'c' / 浮点 3.0f / 3.0d
 *           宽度后缀 :ob :tb :fb :eb（不加则按寄存器宽度或默认 4 字节）
 *   内存：[disp] [base] [base+disp] [base+index] [base+index+disp]
 *   段：section .text | section .data（.text 位于 VA 0，.data 紧随其后）
 *   数据：db dw dd dq  resb resw resd resq
 *   标签：name:          注释：; 或 #
 *   方法（强制）：所有可执行指令必须写在方法体内
 *       method name[(参数)] [-> 返回类型] { ... }   （function / fn 别名）
 *       规则：代码不能裸露在方法外；执行从 main 方法开始
 *       例：
 *           section .text
 *           method main {
 *               ...
 *               intervm 17
 *           }
 */

#include "API-LIB/Pstd/pstd_kapi.h"
#include "API-LIB/Pstd/pstd_string.h"

#define DWN_TYPES_DEFINED
#include "Bin/dawnvm/dwn.h"

#define SRC_MAX   40000u
#define SYM_MAX   512

#define SEC_TEXT  0
#define SEC_DATA  1

static u8  g_out[DWN_HDR_SIZE + DWN_MEM_SIZE + 8] __attribute__((section(".data"))) = {1};
static u8  g_src[SRC_MAX]                          __attribute__((section(".data"))) = {1};

typedef struct {
    char name[40];
    u32  off;          /* 段内偏移 */
    int  sec;
} sym_t;

static sym_t g_syms[SYM_MAX] __attribute__((section(".data"))) = {{1}};
static int   g_nsym;

static int   g_pass;
static int   g_sec;
static u32   g_text_pos;
static u32   g_data_pos;
static u32  *g_cur;         /* 指向当前段的位置计数 */
static u32   g_text_len;
static u32   g_data_len;
static u32   g_data_base;
static int   g_err;
static int   g_line;

/* 规则：所有代码必须包裹在方法内；执行入口固定为 main 方法 */
static int   g_in_method;   /* 是否正处于 method { ... } 内 */
static int   g_have_main;   /* 是否定义了 main 方法 */
static u32   g_main_off;    /* main 方法在 .text 内的偏移 */

static char  g_err_msg[96]  __attribute__((section(".data"))) = {1};
static char  g_err_src[128] __attribute__((section(".data"))) = {1};
static char  g_cur_src[128] __attribute__((section(".data"))) = {1};
static int   g_err_line;

/* ---------------- 错误 ---------------- */

static void asm_err(const char *msg)
{
    int i;

    g_err = 1;
    if (g_err_msg[1])      /* 已记录第一条 */
        return;
    g_err_line = g_line;
    for (i = 0; msg[i] && i < 95; i++)
        g_err_msg[i] = msg[i];
    g_err_msg[i] = 0;
    for (i = 0; g_cur_src[i] && i < 127; i++)
        g_err_src[i] = g_cur_src[i];
    g_err_src[i] = 0;
}

static void asm_err2(const char *a, const char *b)
{
    char m[96];
    int i = 0, j;

    while (a[i] && i < 70)
        m[i] = a[i], i++;
    for (j = 0; b && b[j] && i < 95; j++)
        m[i++] = b[j];
    m[i] = 0;
    asm_err(m);
}

/* ---------------- 字符工具 ---------------- */

static int is_sp(int c)  { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_dig(int c) { return c >= '0' && c <= '9'; }
static int is_alp(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int lc(int c)     { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lc(*a) != lc(*b))
            return 0;
        a++;
        b++;
    }
    return lc(*a) == lc(*b);
}

static char *trim(char *s)
{
    char *e;
    while (*s && is_sp(*s))
        s++;
    e = s + strlen(s);
    while (e > s && is_sp(e[-1]))
        *--e = 0;
    return s;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* ---------------- 符号表 ---------------- */

static sym_t *sym_find(const char *name)
{
    int i;
    for (i = 0; i < g_nsym; i++)
        if (ieq(g_syms[i].name, name))
            return &g_syms[i];
    return 0;
}

static void sym_add(const char *name, u32 off, int sec)
{
    int i;
    sym_t *s;

    if (g_pass != 1)
        return;
    if (sym_find(name)) {
        asm_err2("duplicate label: ", name);
        return;
    }
    if (g_nsym >= SYM_MAX) {
        asm_err("too many symbols");
        return;
    }
    s = &g_syms[g_nsym++];
    for (i = 0; name[i] && i < 39; i++)
        s->name[i] = name[i];
    s->name[i] = 0;
    s->off = off;
    s->sec = sec;
}

static u32 sym_va(const sym_t *s)
{
    return (s->sec == SEC_TEXT ? 0u : g_data_base) + s->off;
}

/* ---------------- 变量（堆） ----------------
 *
 * var-<类型> 名字[, 名字...]  ：类型 i 整数 / f 浮点 / p 指针（元素均 4 字节）
 * 数组写 名字[长度]。变量不是寄存器，收发只走 ldv / stv。
 * 汇编器按声明顺序在堆区分配地址（运行在镜像之后的零初始化内存）。
 */
#define VAR_MAX 128

typedef struct {
    char name[40];
    u32  off;          /* 相对堆基址 */
    int  esz;          /* 元素字节 */
    int  count;        /* 元素个数（标量 = 1） */
    int  is_array;
} var_t;

static var_t g_vars[VAR_MAX] __attribute__((section(".data"))) = {{1}};
static int   g_nvar;
static u32   g_heap_size;
static u32   g_heap_base;

static var_t *var_find(const char *name)
{
    int i;
    for (i = 0; i < g_nvar; i++)
        if (ieq(g_vars[i].name, name))
            return &g_vars[i];
    return 0;
}

static void var_add(const char *name, int esz, int count, int is_array)
{
    int i;
    var_t *v;

    if (g_pass != 1)
        return;
    if (var_find(name)) {
        asm_err2("duplicate var: ", name);
        return;
    }
    if (g_nvar >= VAR_MAX) {
        asm_err("too many vars");
        return;
    }
    v = &g_vars[g_nvar++];
    for (i = 0; name[i] && i < 39; i++)
        v->name[i] = name[i];
    v->name[i] = 0;
    v->off = g_heap_size;
    v->esz = esz;
    v->count = count;
    v->is_array = is_array;
    g_heap_size += (u32)esz * (u32)count;
    g_heap_size = (g_heap_size + 3u) & ~3u;      /* 4 字节对齐 */
}

static u32 var_va(const var_t *v)
{
    return g_heap_base + v->off;
}

/* 解析地址表达式： name | name+num | num ；成功返回 1 */
static int parse_addr(const char *txt, u32 *out)
{
    char buf[64];
    char *p, *q;
    int i = 0;

    while (txt[i] && is_sp(txt[i]))
        i++;
    {
        int j = 0;
        while (txt[i] && !is_sp(txt[i]) && j < 63)
            buf[j++] = txt[i++];
        buf[j] = 0;
    }
    p = buf;
    if (!*p)
        return 0;

    if (is_alp(*p)) {                       /* 符号 [+偏移] */
        sym_t *s;
        q = p;
        while (*q && *q != '+')
            q++;
        if (*q == '+') {
            u32 off = 0, k = 0;
            *q++ = 0;
            while (q[k] && is_dig(q[k]))
                off = off * 10 + (u32)(q[k++] - '0');
            s = sym_find(p);
            if (!s) {
                asm_err2("undefined label: ", p);
                return 0;
            }
            *out = sym_va(s) + off;
            return 1;
        }
        s = sym_find(p);
        if (!s) {
            asm_err2("undefined label: ", p);
            return 0;
        }
        *out = sym_va(s);
        return 1;
    }

    /* 数字 */
    {
        u32 v = 0;
        if (p[0] == '0' && lc(p[1]) == 'x') {
            int k = 2;
            if (hexval(p[k]) < 0) return 0;
            while (hexval(p[k]) >= 0) v = v * 16 + (u32)hexval(p[k++]);
        } else if (p[0] == '0' && lc(p[1]) == 'b') {
            int k = 2;
            if (p[k] != '0' && p[k] != '1') return 0;
            while (p[k] == '0' || p[k] == '1') v = v * 2 + (u32)(p[k++] - '0');
        } else if (is_dig(p[0])) {
            int k = 0;
            while (is_dig(p[k])) v = v * 10 + (u32)(p[k++] - '0');
        } else {
            return 0;
        }
        *out = v;
        return 1;
    }
}

/* ---------------- 输出 ---------------- */

static u32 cur_va(void)
{
    return (g_sec == SEC_TEXT ? 0u : g_data_base) + *g_cur;
}

static void e_b(u32 b)
{
    if (g_pass == 2)
        g_out[DWN_HDR_SIZE + cur_va()] = (u8)b;
    (*g_cur)++;
}

static void e32(u32 v)
{
    e_b(v & 0xFF);
    e_b((v >> 8) & 0xFF);
    e_b((v >> 16) & 0xFF);
    e_b((v >> 24) & 0xFF);
}

static void e64(u64 v)
{
    e32((u32)v);
    e32((u32)(v >> 32));
}

static u32 width_to_size(int bytes)
{
    return bytes == 1 ? DWN_SIZE_OB
         : bytes == 2 ? DWN_SIZE_TB
         : bytes == 8 ? DWN_SIZE_EB
         : DWN_SIZE_FB;
}

/* 32 位寄存器的一元/无参指令映射到 2 字节短指令码 */
static u32 short_of(u32 op)
{
    switch (op) {
    case OP_INC:  return OP_S_INC;
    case OP_DEC:  return OP_S_DEC;
    case OP_NOT:  return OP_S_NOT;
    case OP_NEG:  return OP_S_NEG;
    case OP_PUSH: return OP_S_PUSH;
    case OP_POP:  return OP_S_POP;
    case OP_MOV:  return OP_S_MOV;
    }
    return op;
}

/* ---------------- 操作数 ---------------- */

enum { K_REG, K_PAIR, K_IMM, K_MEM };

typedef struct {
    int  kind;
    int  reg;          /* 寄存器 / 对基址 */
    int  width;        /* 字节：1/2/4/8 */
    /* 立即数 */
    u64  val;
    int  val_known;    /* pass2 用 */
    char sym[64];
    int  has_sym;
    u32  off;          /* 符号 + 偏移 */
    int  width_set;
    /* 内存 */
    int  base, index;
    int  scale;        /* 变址缩放：1/2/4/8 */
    u32  disp;
    char dsym[64];
    int  dhas_sym;
} opnd_t;

/* stv / 变址缩放借用的临时寄存器 */
#define DWN_SCRATCH  12
#define DWN_SCRATCH2 13

/* loop 糖的步长（endloop 时用） */
typedef struct {
    int kind;          /* K_REG / K_IMM */
    int reg;
    u32 val;
} lstep_t;

#define LOOP_MAX 16
static int     g_loop_depth;
static int     g_loop_next;
static int     g_loop_id[LOOP_MAX]   __attribute__((section(".data"))) = {1};
static int     g_loop_reg[LOOP_MAX]  __attribute__((section(".data"))) = {1};
static int     g_loop_w[LOOP_MAX]    __attribute__((section(".data"))) = {1};
static lstep_t g_loop_step[LOOP_MAX] __attribute__((section(".data"))) = {{1}};

/* 解析寄存器视图名，返回消耗的字符数（0 = 不是寄存器） */
static int parse_reg(const char *s, int *pw, int *pr)
{
    int w;

    if ((s[0] == 'r' || s[0] == 'R')) {
        if (strncmp(s + 1, "sp", 2) == 0) { *pw = 4; *pr = DWN_SP; return 3; }
        if (strncmp(s + 1, "SP", 2) == 0) { *pw = 4; *pr = DWN_SP; return 3; }
        if (strncmp(s + 1, "fp", 2) == 0) { *pw = 4; *pr = DWN_FP; return 3; }
        if (strncmp(s + 1, "FP", 2) == 0) { *pw = 4; *pr = DWN_FP; return 3; }
        {
            int k = 1, v = 0, n = 0;
            while (s[k] && n < 2 && hexval(s[k]) >= 0) {
                v = v * 16 + hexval(s[k]);
                k++; n++;
            }
            if (n == 0 || v >= DWN_NREG)
                return 0;
            *pw = 4; *pr = v;
            return k;
        }
    }

    {
        int c = lc(s[0]);
        if (c == 'e') w = 4;
        else if (c == 'x') w = 2;
        else if (c == 'h' || c == 'l') w = 1;
        else return 0;
    }
    {
        int t = lc(s[1]);
        int h;
        if (t != 'v' && t != 'i' && t != 'f' && t != 'p')
            return 0;
        h = hexval(s[2]);
        if (h < 0)
            return 0;
        *pw = w;
        *pr = h;
        return 3;
    }
}

static int reg_full(const char *s, int *pw, int *pr)
{
    int n = parse_reg(s, pw, pr);
    return n > 0 && s[n] == 0;
}

static void parse_width_suffix(opnd_t *o, const char *w)
{
    o->width_set = 1;
    if (ieq(w, "ob") || ieq(w, "o") || ieq(w, "b")) o->width = 1;
    else if (ieq(w, "tb") || ieq(w, "t"))           o->width = 2;
    else if (ieq(w, "eb") || ieq(w, "e"))           o->width = 8;
    else if (ieq(w, "fb") || ieq(w, "f"))           o->width = 4;
    else asm_err2("bad width suffix: ", w);
}

static int dec_ieee(const char *v, int len, int want_double, u64 *out);

static void parse_value(opnd_t *o, const char *v, const char *wsuf)
{
    char tmp[64];
    int i = 0;

    while (v[i] && is_sp(v[i]))
        i++;
    {
        int j = 0;
        while (v[i] && !is_sp(v[i]) && j < 63)
            tmp[j++] = v[i++];
        tmp[j] = 0;
    }
    v = tmp;

    if (wsuf)
        parse_width_suffix(o, wsuf);

    if (v[0] == '\'') {                     /* 字符字面量 */
        int c = v[1];
        if (c == '\\') {
            switch (lc(v[2])) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = 0;    break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            default: c = v[2]; break;
            }
        }
        o->val = (u32)c;
        o->val_known = 1;
        if (!o->width_set) o->width = 1;
        return;
    }

    /* 浮点：含 '.' 或 以 f/d 结尾（排除 0x.. / 0b.. 这类整数写法） */
    {
        int start = (v[0] == '-' || v[0] == '+') ? 1 : 0;
        int L = (int)strlen(v);
        int isd;

        if ((is_dig(v[start]) || v[start] == '.') &&
            !strchr(v, 'x') && !strchr(v, 'X') &&
            !strchr(v, 'b') && !strchr(v, 'B') &&
            (strchr(v, '.') ||
             (L > start && (lc(v[L - 1]) == 'f' || lc(v[L - 1]) == 'd')))) {
            u64 bits;
            isd = (L > 0 && lc(v[L - 1]) == 'd');
            if (!dec_ieee(v, L, isd, &bits)) {
                asm_err2("bad float: ", v);
                return;
            }
            o->val = bits;
            o->val_known = 1;
            if (!o->width_set) o->width = isd ? 8 : 4;
            return;
        }
    }

    {
        int neg = 0, si = 0;
        if (v[0] == '-') { neg = 1; si = 1; }
        else if (v[0] == '+') { si = 1; }
        if (is_dig(v[si]) || v[si] == '0') {    /* 数字（可带正负号） */
            u32 n = 0;
            if (!parse_addr(v + si, &n)) {
                asm_err2("bad number: ", v);
                return;
            }
            o->val = neg ? (u32)(-(i32)n) : n;
            o->val_known = 1;
            return;
        }
    }

    if (is_alp(v[0])) {                     /* 符号 */
        int i2 = 0;
        while (v[i2] && v[i2] != '+' && i2 < 63) {
            o->sym[i2] = v[i2];
            i2++;
        }
        o->sym[i2] = 0;
        o->has_sym = 1;
        if (v[i2] == '+') {
            int k = i2 + 1;
            while (is_dig(v[k]))
                o->off = o->off * 10 + (u32)(v[k++] - '0');
        }
        return;
    }

    asm_err2("bad operand: ", v);
}

static void parse_mem(opnd_t *o, const char *inner)
{
    char buf[96];
    int i = 0;

    while (inner[i] && is_sp(inner[i]))
        i++;
    {
        int j = 0;
        while (inner[i] && inner[i] != ']' && j < 95)
            buf[j++] = inner[i++];
        buf[j] = 0;
    }

    o->base = -1;
    o->index = -1;
    o->scale = 1;

    {
        char *p = buf;
        while (*p) {
            char *q = p;
            char part[64];
            int k = 0;
            while (*q && *q != '+' && k < 63)
                part[k++] = *q++;
            part[k] = 0;
            if (*q == '+')
                q++;
            {
                char *t = trim(part);
                char *star;
                int rw, rr, n;
                if (!*t) { p = q; continue; }
                /* 变址缩放：reg*N（N = 1/2/4/8） */
                star = strchr(t, '*');
                if (star)
                    *star = 0;
                t = trim(t);
                n = parse_reg(t, &rw, &rr);
                if (n > 0 && t[n] == 0) {
                    if (o->base < 0)       o->base = rr;
                    else if (o->index < 0) o->index = rr;
                    else asm_err("too many registers in address");
                    if (star) {
                        char *sc = trim(star + 1);
                        int sv = 0;
                        while (is_dig(*sc))
                            sv = sv * 10 + (*sc++ - '0');
                        if (*sc || (sv != 1 && sv != 2 && sv != 4 && sv != 8))
                            asm_err("bad index scale (1/2/4/8)");
                        else
                            o->scale = sv;
                    }
                } else {
                    opnd_t d;
                    memset(&d, 0, sizeof(d));
                    parse_value(&d, t, 0);
                    if (d.has_sym) {
                        int z = 0;
                        while (d.sym[z]) { o->dsym[z] = d.sym[z]; z++; }
                        o->dsym[z] = 0;
                        o->dhas_sym = 1;
                        o->disp = d.off;
                    } else {
                        o->disp = (u32)d.val;
                    }
                }
            }
            p = q;
        }
    }
}

static void parse_operand(const char *s, opnd_t *o)
{
    char buf[96];
    char *t;
    int i = 0;

    memset(o, 0, sizeof(*o));
    o->base = o->index = -1;
    o->width = 4;
    o->scale = 1;

    while (s[i] && is_sp(s[i]))
        i++;
    {
        int j = 0;
        while (s[i] && j < 95)
            buf[j++] = s[i++];
        buf[j] = 0;
    }
    t = trim(buf);
    if (!*t)
        return;

    if (t[0] == '[') {
        o->kind = K_MEM;
        parse_mem(o, t + 1);
        return;
    }

    {
        char *colon = strchr(t, ':');
        if (colon) {
            char left[64], right[64];
            int n = (int)(colon - t);
            int w1, r1, w2, r2;
            for (i = 0; i < n && i < 63; i++) left[i] = t[i];
            left[i] = 0;
            {
                int j = 0;
                char *rp = colon + 1;
                while (*rp && j < 63) right[j++] = *rp++;
                right[j] = 0;
            }
            if (reg_full(trim(left), &w1, &r1) && reg_full(trim(right), &w2, &r2)) {
                if (r2 != r1 + 1)
                    asm_err("register pair must be consecutive, low first");
                o->kind = K_PAIR;
                o->reg = r1;
                o->width = 8;
                return;
            }
            parse_value(o, trim(left), trim(right));
            o->kind = K_IMM;
            return;
        }
    }

    {
        int w, r;
        if (reg_full(t, &w, &r)) {
            o->kind = K_REG;
            o->reg = r;
            o->width = w;
            return;
        }
    }

    parse_value(o, t, 0);
    o->kind = K_IMM;
}

/* 解析变量表达式： name | name[下标寄存器] */
static int parse_varexpr(const char *s, char *name, int namesz,
                         int *has_idx, opnd_t *idx)
{
    char buf[96];
    int i = 0, k = 0;

    *has_idx = 0;
    while (s[i] && is_sp(s[i]))
        i++;
    while (s[i] && (is_alp(s[i]) || is_dig(s[i]) || s[i] == '.') && k < namesz - 1)
        name[k++] = s[i++];
    name[k] = 0;
    if (k == 0)
        return 0;
    while (s[i] && is_sp(s[i]))
        i++;
    if (s[i] == '[') {
        int j = 0;
        i++;
        while (s[i] && s[i] != ']' && j < 95)
            buf[j++] = s[i++];
        buf[j] = 0;
        if (s[i] != ']')
            return 0;
        parse_operand(buf, idx);
        if (idx->kind != K_REG) {
            asm_err("array index must be a register");
            return 0;
        }
        *has_idx = 1;
    }
    return 1;
}

static void resolve_imm(opnd_t *o, u32 *out)
{
    if (o->has_sym) {
        sym_t *s = sym_find(o->sym);
        if (!s) {
            asm_err2("undefined label: ", o->sym);
            *out = 0;
            return;
        }
        *out = sym_va(s) + o->off;
    } else {
        *out = (u32)o->val;
    }
}

/* ---------------- 指令发射 ---------------- */

static void emit_imm(u32 op, u32 dst, u32 size, opnd_t *imm)
{
    u32 n = DWN_SIZE_BYTES(size);
    u32 i;
    u32 v = 0;

    e_b(op);
    e_b(DWN_MK_MOD(dst, size, DWN_KIND_IMM));
    if (g_pass == 2)
        resolve_imm(imm, &v);
    for (i = 0; i < n; i++)
        e_b((v >> (8 * i)) & 0xFF);
}

static void emit_rrr(u32 op, u32 dst, u32 a, u32 b, u32 size)
{
    e_b(op);
    e_b(DWN_MK_MOD(dst, size, DWN_KIND_RRR));
    e_b((a << 4) | (b & 0xF));
}

static void emit_mem(u32 op, u32 reg, u32 size, opnd_t *m, int is_store)
{
    u32 base = (m->base < 0) ? 0xF : (u32)m->base;
    u32 idx  = (m->index < 0) ? 0xF : (u32)m->index;
    u32 disp = 0;

    e_b(op);
    e_b(DWN_MK_MOD(reg, size, DWN_KIND_MEM));
    e_b((base << 4) | (idx & 0xF));
    if (g_pass == 2) {
        if (m->dhas_sym) {
            sym_t *s = sym_find(m->dsym);
            if (!s) {
                asm_err2("undefined label: ", m->dsym);
            } else {
                disp = sym_va(s) + m->disp;
            }
        } else {
            disp = m->disp;
        }
    }
    e32(disp);
    (void)is_store;
}

static void emit_branch(u32 op, int cc, const char *target)
{
    u32 va = cur_va();

    e_b(op);
    e_b(DWN_MK_MOD_CC(cc));
    if (g_pass == 2) {
        u32 tgt = 0;
        if (!parse_addr(target, &tgt))
            asm_err2("bad branch target: ", target);
        e32(tgt - (va + 6));
    } else {
        e32(0);
    }
}

static void emit_svc(u32 id)
{
    e_b(OP_SVC);
    e_b(DWN_MK_MOD_CC(CC_AL));
    e_b(id & 0xFF);
}

/* 系统调用名 -> 调用号：intervm 的位置 0 可以写名字，免得记编号。
 * 名字大小写不敏感；找不到返回 -1。 */
static u32 svc_by_name(const char *n)
{
    static const struct { const char *n; u32 id; } t[] = {
        { "PUTC",      DWN_SVC_PUTC      },
        { "PRINT",     DWN_SVC_PRINT     },
        { "READLINE",  DWN_SVC_READLINE  },
        { "READLINES", DWN_SVC_READLINES },
        { "PRINT_I32", DWN_SVC_PRINT_I32 },
        { "PRINT_U32", DWN_SVC_PRINT_U32 },
        { "PRINT_HEX", DWN_SVC_PRINT_HEX },
        { "PRINT_F32", DWN_SVC_PRINT_F32 },
        { "GETKEY",    DWN_SVC_GETKEY    },
        { "STRLEN",    DWN_SVC_STRLEN    },
        { "STRCMP",    DWN_SVC_STRCMP    },
        { "STRCPY",    DWN_SVC_STRCPY    },
        { "ATOI",      DWN_SVC_ATOI      },
        { "ITOA",      DWN_SVC_ITOA      },
        { "UTOA",      DWN_SVC_UTOA      },
        { "FTOA",      DWN_SVC_FTOA      },
        { "ATOF",      DWN_SVC_ATOF      },
        { "EXIT",      DWN_SVC_EXIT      },
        { "SETMODE",   DWN_SVC_SETMODE   },
        { "GETMODE",   DWN_SVC_GETMODE   },
        { "TOUPPER",   DWN_SVC_TOUPPER   },
        { "TOLOWER",   DWN_SVC_TOLOWER   },
        { "STRUPR",    DWN_SVC_STRUPR    },
        { "STRLWR",    DWN_SVC_STRLWR    },
        { "SORT_I32",  DWN_SVC_SORT_I32  },
        { "SORT_I32D", DWN_SVC_SORT_I32D },
        { "SORT_F32",  DWN_SVC_SORT_F32  },
        { "SORT_F32D", DWN_SVC_SORT_F32D },
        { "MEMZERO",   DWN_SVC_MEMZERO   },
        { "PRINTC",    DWN_SVC_PRINTC    },
        { "GOTOXY",    DWN_SVC_GOTOXY    },
        { "CLEAR",     DWN_SVC_CLEAR     },
        { 0, 0 }
    };
    int i;

    for (i = 0; t[i].n; i++)
        if (ieq(n, t[i].n))
            return t[i].id;
    return 0xFFFFFFFFu;
}

/* 解析 intervm 的 id：优先当标签，其次当系统调用名，最后当立即数 */
static void resolve_svc_id(opnd_t *o, u32 *out)
{
    if (o->has_sym && !sym_find(o->sym)) {
        u32 id = svc_by_name(o->sym);
        if (id != 0xFFFFFFFFu) {
            *out = id;
            return;
        }
    }
    resolve_imm(o, out);
}

/* ---------------- v4 语法辅助 ---------------- */

/* 2 字节短指令 [op][dst<<4] */
static void emit_short(u32 op, u32 reg)
{
    e_b(op);
    e_b(((reg & 0xF) << 4));
}

/* 立即数（值已知，非符号） */
static void emit_imm_val(u32 op, u32 dst, u32 size, u32 val)
{
    opnd_t o;
    memset(&o, 0, sizeof(o));
    o.base = o.index = -1;
    o.width = 4;
    o.val = val;
    o.val_known = 1;
    emit_imm(op, dst, size, &o);
}

/* 访存：绝对地址 / 变址（disp 为绝对地址，base 为变址寄存器） */
static void emit_mem_abs(u32 op, u32 reg, u32 size, u32 addr, int store)
{
    opnd_t m;
    memset(&m, 0, sizeof(m));
    m.kind = K_MEM;
    m.base = m.index = -1;
    m.disp = addr;
    m.width = 4;
    emit_mem(op, reg, size, &m, store);
}

static void emit_mem_idx(u32 op, u32 reg, u32 size, u32 ireg, u32 addr, int store)
{
    opnd_t m;
    memset(&m, 0, sizeof(m));
    m.kind = K_MEM;
    m.base = (int)ireg;
    m.index = -1;
    m.disp = addr;
    m.width = 4;
    emit_mem(op, reg, size, &m, store);
}

/* 变址缩放：编码里没有 scale 位，展开成移位到 scratch 再寻址 */
static void mem_apply_scale(opnd_t *m)
{
    int sh;
    u32 s;

    if (m->index < 0 || m->scale <= 1)
        return;
    sh = 0;
    for (s = (u32)m->scale; s > 1; s >>= 1)
        sh++;
    emit_rrr(OP_MOV, DWN_SCRATCH, (u32)m->index, 0, DWN_SIZE_FB);
    emit_imm_val(OP_SHL, DWN_SCRATCH, DWN_SIZE_FB, (u32)sh);
    m->index = DWN_SCRATCH;
}

static int is_null_opnd(const opnd_t *o)
{
    return o->kind == K_IMM && o->has_sym &&
           (ieq(o->sym, "NULL") || ieq(o->sym, "null"));
}
static void emit_arg_to(u32 reg, const opnd_t *o)
{
    if (o->kind == K_REG) {
        if (o->reg == (int)reg)
            return;
        emit_rrr(OP_MOV, reg, (u32)o->reg, 0, DWN_SIZE_FB);
    } else if (o->kind == K_IMM) {
        emit_imm(OP_LDI, reg, o->width_set ? width_to_size(o->width) : DWN_SIZE_FB, (opnd_t *)o);
    } else {
        asm_err("bad argument (need register/immediate)");
    }
}

/* 系统调用返回值 R0 -> dst 寄存器 */
static void emit_ret_from(u32 reg, const opnd_t *d)
{
    if (d->kind == K_REG) {
        if (d->reg == (int)reg)
            return;
        emit_rrr(OP_MOV, (u32)d->reg, reg, 0, DWN_SIZE_FB);
    } else {
        asm_err("return target must be a register");
    }
}

/* 把操作数（寄存器/立即数/符号）装进指定寄存器 */
static void emit_load_reg(u32 dst, opnd_t *o)
{
    if (o->kind == K_REG) {
        if (o->reg != (int)dst)
            emit_rrr(OP_MOV, dst, (u32)o->reg, 0, DWN_SIZE_FB);
    } else {
        emit_imm(OP_LDI, dst, o->width_set ? width_to_size(o->width) : DWN_SIZE_FB, o);
    }
}

/* 数组元素字节 -> 变址移位量（esz 为 1/2/4/8 时返回 log2，否则 -1） */
static int elem_shift(int esz)
{
    switch (esz) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    }
    return -1;
}

/* loop 标签：__loop<id>_top / __loop<id>_end */
static void loop_label(char *out, int id, int end)
{
    static const char *pre = "__loop";
    char num[12];
    int n = 0, i, k = 0;

    while (pre[k]) { out[k] = pre[k]; k++; }
    if (id == 0)
        num[n++] = '0';
    while (id > 0) { num[n++] = (char)('0' + id % 10); id /= 10; }
    for (i = n - 1; i >= 0; i--)
        out[k++] = num[i];
    out[k++] = '_';
    if (end) { out[k++] = 'e'; out[k++] = 'n'; out[k++] = 'd'; }
    else     { out[k++] = 't'; out[k++] = 'o'; out[k++] = 'p'; }
    out[k] = 0;
}

/* 变量声明：var-<类型> 名字[, 名字...]，数组写 名字[长度] */
static void do_var(const char *mn, char *rest)
{
    char t = (char)lc((unsigned char)mn[4]);
    char *p = rest;

    if (t != 'i' && t != 'f' && t != 'p') {
        asm_err2("bad var type: ", mn);
        return;
    }
    for (;;) {
        char name[40];
        int k = 0;
        while (*p && (is_sp(*p) || *p == ','))
            p++;
        if (!*p)
            break;
        while (*p && (is_alp(*p) || is_dig(*p) || *p == '.') && k < 39)
            name[k++] = *p++;
        name[k] = 0;
        if (k == 0) {
            asm_err("var: bad name");
            return;
        }
        if (*p == '[') {
            int n = 0;
            p++;
            while (is_dig(*p))
                n = n * 10 + (*p++ - '0');
            if (*p == ']')
                p++;
            if (n <= 0) {
                asm_err("var: bad array size");
                return;
            }
            var_add(name, 4, n, 1);
        } else {
            var_add(name, 4, 1, 0);
        }
    }
}

/* 前置声明：loop 糖在定义处之前就要用到这几个 */
static int  split_ops(char *s, char *ops[], int max);
static int  cond_cc(const char *n);
static int  cc_inv(int cc);

/* 循环糖：
 *   loop{REG, start, <cc>, a, n}
 *       ...
 *   endloop
 * 展开为
 *   REG = start
 * __loopN_top:
 *   cmp REG, a
 *   j<inv cc> __loopN_end
 *   ...body...
 *   REG += n
 *   jmp __loopN_top
 * __loopN_end:
 */
static void do_loop(char *rest)
{
    char buf[160];
    char *toks[8];
    char lb[40];
    int i = 0, k = 0, nt, w, reg, cc, dpt, id;
    char *close;
    opnd_t start, cmpv, step;

    memset(&start, 0, sizeof(start));
    memset(&cmpv, 0, sizeof(cmpv));
    memset(&step, 0, sizeof(step));

    while (rest[i] && is_sp(rest[i]))
        i++;
    if (rest[i] != '{') {
        asm_err("loop: expected '{'");
        return;
    }
    while (rest[i] && k < 159)
        buf[k++] = rest[i++];
    buf[k] = 0;
    close = strchr(buf, '}');
    if (!close) {
        asm_err("loop: missing '}'");
        return;
    }
    *close = 0;
    nt = split_ops(buf + 1, toks, 8);
    if (nt < 5) {
        asm_err("loop: need {REG, start, cc, a, n}");
        return;
    }
    {
        int n = parse_reg(toks[0], &w, &reg);
        if (n <= 0 || toks[0][n] != 0) {
            asm_err("loop: bad counter register");
            return;
        }
    }
    cc = cond_cc(toks[2]);
    if (cc < 0) {
        asm_err2("loop: bad cc: ", toks[2]);
        return;
    }
    parse_operand(toks[1], &start);
    parse_operand(toks[3], &cmpv);
    parse_operand(toks[4], &step);

    if (g_loop_depth >= LOOP_MAX) {
        asm_err("loop: too deep");
        return;
    }
    id = g_loop_next++;
    dpt = g_loop_depth++;
    g_loop_id[dpt] = id;
    g_loop_reg[dpt] = reg;
    g_loop_w[dpt] = w;
    g_loop_step[dpt].kind = step.kind;
    g_loop_step[dpt].reg = step.reg;
    g_loop_step[dpt].val = (u32)step.val;

    /* REG = start */
    if (start.kind == K_REG)
        emit_rrr(OP_MOV, (u32)reg, (u32)start.reg, 0, width_to_size(w));
    else
        emit_imm(OP_LDI, (u32)reg, width_to_size(w), &start);

    /* 循环顶 */
    loop_label(lb, id, 0);
    sym_add(lb, *g_cur, g_sec);

    /* cmp REG, a */
    if (cmpv.kind == K_IMM)
        emit_imm(OP_CMP, (u32)reg, width_to_size(w), &cmpv);
    else
        emit_rrr(OP_CMP, (u32)reg, (u32)reg, (u32)cmpv.reg, width_to_size(w));

    /* 条件不成立 -> end */
    loop_label(lb, id, 1);
    emit_branch(OP_JCC, cc_inv(cc), lb);
}

static void do_endloop(void)
{
    char lb[40];
    int dpt, id, reg, w;
    lstep_t *st;

    if (g_loop_depth <= 0) {
        asm_err("endloop without loop");
        return;
    }
    dpt = --g_loop_depth;
    id  = g_loop_id[dpt];
    reg = g_loop_reg[dpt];
    w   = g_loop_w[dpt];
    st  = &g_loop_step[dpt];

    /* REG += n （n 有符号，负数用 sub） */
    if (st->kind == K_REG) {
        emit_rrr(OP_ADD, (u32)reg, (u32)reg, (u32)st->reg, width_to_size(w));
    } else {
        if ((i32)st->val < 0)
            emit_imm_val(OP_SUB, (u32)reg, width_to_size(w), (u32)(-(i32)st->val));
        else
            emit_imm_val(OP_ADD, (u32)reg, width_to_size(w), st->val);
    }

    loop_label(lb, id, 0);
    emit_branch(OP_JMP, CC_AL, lb);
    loop_label(lb, id, 1);
    sym_add(lb, *g_cur, g_sec);
}

/* ldv dst, 变量  |  ldv dst, 数组[下标] */
static void do_ldv(char *dsttxt, char *vartxt)
{
    char vname[64];
    int has_idx, sh;
    opnd_t idx, d;
    var_t *v;

    parse_operand(dsttxt, &d);
    if (d.kind != K_REG && d.kind != K_PAIR) {
        asm_err("ldv: dst must be a register");
        return;
    }
    if (!parse_varexpr(vartxt, vname, 64, &has_idx, &idx))
        return;
    v = var_find(vname);
    if (!v) {
        asm_err2("undefined var: ", vname);
        return;
    }
    if (!has_idx) {
        if (v->is_array)      /* 不带下标 -> 取首地址 */
            emit_imm_val(OP_LDI, (u32)d.reg, DWN_SIZE_FB, var_va(v));
        else
            emit_mem_abs(OP_LD, (u32)d.reg, width_to_size(d.width), var_va(v), 0);
        return;
    }
    sh = elem_shift(v->esz);
    if (sh < 0) {
        asm_err("ldv: bad element size");
        return;
    }
    emit_rrr(OP_MOV, DWN_SCRATCH, (u32)idx.reg, 0, DWN_SIZE_FB);
    if (sh > 0)
        emit_imm_val(OP_SHL, DWN_SCRATCH, DWN_SIZE_FB, (u32)sh);
    emit_mem_idx(OP_LD, (u32)d.reg, width_to_size(d.width), DWN_SCRATCH, var_va(v), 0);
}

/* stv 变量, src  |  stv 数组[下标], src */
static void do_stv(char *vartxt, char *srctxt)
{
    char vname[64];
    int has_idx, sh;
    opnd_t idx, s;
    var_t *v;
    u32 srcreg, size;

    if (!parse_varexpr(vartxt, vname, 64, &has_idx, &idx))
        return;
    v = var_find(vname);
    if (!v) {
        asm_err2("undefined var: ", vname);
        return;
    }
    parse_operand(srctxt, &s);
    size = (s.kind == K_REG) ? width_to_size(s.width) : DWN_SIZE_FB;
    srcreg = (u32)s.reg;

    if (has_idx) {
        sh = elem_shift(v->esz);
        if (sh < 0) {
            asm_err("stv: bad element size");
            return;
        }
        emit_rrr(OP_MOV, DWN_SCRATCH2, (u32)idx.reg, 0, DWN_SIZE_FB);
        if (sh > 0)
            emit_imm_val(OP_SHL, DWN_SCRATCH2, DWN_SIZE_FB, (u32)sh);
    }
    if (s.kind != K_REG) {
        emit_load_reg(DWN_SCRATCH, &s);
        srcreg = DWN_SCRATCH;
        size = DWN_SIZE_FB;
    }
    if (has_idx)
        emit_mem_idx(OP_ST, srcreg, size, DWN_SCRATCH2, var_va(v), 1);
    else
        emit_mem_abs(OP_ST, srcreg, size, var_va(v), 1);
}

/* ---------------- 条件码 ---------------- */

static int cond_cc(const char *n)
{
    static const struct { const char *n; int cc; } T[] = {
        {"e",CC_EQ},{"z",CC_EQ},{"eq",CC_EQ},
        {"ne",CC_NE},{"nz",CC_NE},
        {"l",CC_LT},{"lt",CC_LT},{"il",CC_LT},{"nge",CC_LT},
        {"le",CC_LE},{"ile",CC_LE},{"ng",CC_LE},
        {"g",CC_GT},{"gt",CC_GT},{"ig",CC_GT},{"nle",CC_GT},
        {"ge",CC_GE},{"ige",CC_GE},{"nl",CC_GE},
        {"b",CC_LTU},{"c",CC_LTU},{"ltu",CC_LTU},{"ul",CC_LTU},{"nae",CC_LTU},
        {"be",CC_LEU},{"leu",CC_LEU},{"ule",CC_LEU},{"na",CC_LEU},
        {"a",CC_GTU},{"gtu",CC_GTU},{"ug",CC_GTU},{"nbe",CC_GTU},
        {"ae",CC_GEU},{"geu",CC_GEU},{"uge",CC_GEU},{"nb",CC_GEU},{"nc",CC_GEU},
        {"s",CC_S},{"ns",CC_NS},{"o",CC_O},
        {"al",CC_AL},
        {0,0}
    };
    int i;
    for (i = 0; T[i].n; i++)
        if (ieq(T[i].n, n))
            return T[i].cc;
    return -1;
}

/* 条件取反（loop 糖用：循环条件成立时继续，不成立跳到 end） */
static int cc_inv(int cc)
{
    switch (cc) {
    case CC_EQ:  return CC_NE;
    case CC_NE:  return CC_EQ;
    case CC_LT:  return CC_GE;
    case CC_LE:  return CC_GT;
    case CC_GT:  return CC_LE;
    case CC_GE:  return CC_LT;
    case CC_LTU: return CC_GEU;
    case CC_LEU: return CC_GTU;
    case CC_GTU: return CC_LEU;
    case CC_GEU: return CC_LTU;
    case CC_S:   return CC_NS;
    case CC_NS:  return CC_S;
    case CC_C:   return CC_NC;
    case CC_NC:  return CC_C;
    default:     return CC_AL;      /* al / o 无合适取反，退化为无条件 */
    }
}

/* 找顶层 "->"（不在 [] 内）；返回其位置，找不到返回 0 */
static char *find_arrow(char *s)
{
    int d = 0;
    for (; *s; s++) {
        if (*s == '[') d++;
        else if (*s == ']') d--;
        else if (d == 0 && s[0] == '-' && s[1] == '>')
            return s;
    }
    return 0;
}

/* ---------------- 操作数拆分 ---------------- */

static int split_ops(char *s, char *ops[], int max)
{
    int n = 0, depth = 0;
    char *p = s;
    char *start = s;

    for (;;) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        if ((*p == ',' && depth == 0) || *p == 0) {
            int end = (*p == 0);
            *p = 0;
            if (n < max) {
                ops[n] = trim(start);
                if (ops[n][0])
                    n++;
            }
            if (end)
                break;
            start = p + 1;
        }
        p++;
    }
    return n;
}

/* ---------------- 段 ---------------- */

/* 记录当前段的最终长度：每次切段以及收尾时都要调用，
 * 否则只会记到最后出现的那个段，.data 或 .text 之一会被漏掉。 */
static void close_section(void)
{
    if (g_sec == SEC_TEXT)
        g_text_len = g_text_pos;
    else
        g_data_len = g_data_pos;
}

static void set_section(char *nm)
{
    close_section();

    if (ieq(nm, ".data") || ieq(nm, "data")) {
        g_sec = SEC_DATA;
        g_cur = &g_data_pos;
    } else if (ieq(nm, ".text") || ieq(nm, "text") || ieq(nm, "code")) {
        g_sec = SEC_TEXT;
        g_cur = &g_text_pos;
    } else {
        asm_err2("unknown section: ", nm);
    }
}

/* ---------------- 数据伪指令 ---------------- */

static void emit_dstr(char *rest)
{
    char *p = rest;
    for (;;) {
        while (*p && (is_sp(*p) || *p == ','))
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                int c = *p++;
                if (c == '\\' && *p) {
                    switch (lc(*p)) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case '0': c = 0;    break;
                    case '\\': c = '\\'; break;
                    case '"': c = '"';  break;
                    default: c = *p; break;
                    }
                    p++;
                }
                e_b((u8)c);
            }
            if (*p == '"')
                p++;
        } else {
            opnd_t d;
            char tok[64];
            int i = 0;
            while (*p && !is_sp(*p) && *p != ',' && i < 63)
                tok[i++] = *p++;
            tok[i] = 0;
            memset(&d, 0, sizeof(d));
            parse_value(&d, tok, 0);
            e_b((u32)d.val & 0xFF);
        }
    }
}

static void emit_data(const char *dir, char *rest)
{
    char *p = rest;
    int w = 1;

    if (ieq(dir, "dw")) w = 2;
    else if (ieq(dir, "dd")) w = 4;
    else if (ieq(dir, "dq")) w = 8;

    for (;;) {
        int i;
        while (*p && (is_sp(*p) || *p == ','))
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                int c = *p++;
                if (c == '\\' && *p) {
                    switch (lc(*p)) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case '0': c = 0;    break;
                    case '\\': c = '\\'; break;
                    case '"': c = '"';  break;
                    default: c = *p; break;
                    }
                    p++;
                }
                e_b((u8)c);
            }
            if (*p == '"')
                p++;
            continue;
        }
        {
            opnd_t d;
            char tok[64];
            int k = 0;
            while (*p && !is_sp(*p) && *p != ',' && k < 63)
                tok[k++] = *p++;
            tok[k] = 0;
            memset(&d, 0, sizeof(d));
            parse_value(&d, tok, 0);
            for (i = 0; i < w; i++)
                e_b(((u32)d.val >> (8 * i)) & 0xFF);
        }
    }
}

static void emit_res(char *rest, int esz)
{
    u32 n = 0;
    char *p = trim(rest);
    while (is_dig(*p))
        n = n * 10 + (u32)(*p++ - '0');
    n *= (u32)esz;
    while (n--)
        e_b(0);
}

/* ---------------- 单行 ---------------- */

/* 64 位无符号除以 10，用逐位移位长除法，避免 i386 上的 __udivdi3。
 * *rem 非空时回填余数。 */
static u64 udiv10(u64 x, u32 *rem)
{
    u64 q = 0;
    u32 r = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        r = (r << 1) | (u32)((x >> i) & 1u);
        if (r >= 10u) {
            r -= 10u;
            q |= (1ull << i);
        }
    }
    if (rem)
        *rem = r;
    return q;
}

/* 十进制浮点字面量 -> IEEE-754 位型（纯整数运算，目标机没有 FPU，
 * 用 double 会拖进 soft-float 运行时，所以自己算）。
 * v 形如 [±]digits[.digits][f|d]；want_double=1 出 float64，否则 float32。 */
static int dec_ieee(const char *v, int len, int want_double, u64 *out)
{
    int i = 0, neg = 0, seen_dot = 0, nd = 0, e10 = 0;
    u64 mant = 0, sig;
    int scale = 0, prec = want_double ? 53 : 24;
    int mbias = want_double ? 52 : 23;
    int ebits = want_double ? 11 : 8;
    int xbias = want_double ? 1023 : 127;
    u64 signbit = want_double ? 0x8000000000000000ull : 0x80000000ull;

    if (i < len && (v[i] == '-' || v[i] == '+')) {
        neg = (v[i] == '-');
        i++;
    }
    for (; i < len; i++) {
        char c = v[i];
        if (c == '.') { seen_dot = 1; continue; }
        if (c == 'f' || c == 'F' || c == 'd' || c == 'D') break;
        if (c < '0' || c > '9') return 0;
        if (nd < 18) {
            mant = mant * 10u + (u64)(c - '0');
            nd++;
            if (seen_dot) e10--;
        } else if (!seen_dot) {
            e10++;                  /* 超出精度的整数位：整体放大 10 倍 */
        }
        /* 超出精度的小数位：丢弃 */
    }

    if (mant == 0) { *out = neg ? signbit : 0; return 1; }

    /* value = mant * 10^e10，归一到 sig * 2^scale（sig 约 63 位有效） */
    sig = mant;
    if (e10 > 0) {
        int k;
        for (k = 0; k < e10; k++) {
            if (sig <= 0xFFFFFFFFFFFFFFFFull / 10u)
                sig *= 10u;
            else
                sig = (sig >> 1) * 10u, scale++;
        }
    } else if (e10 < 0) {
        int k;
        for (k = 0; k < -e10; k++) {
            while (sig <= 0xFFFFFFFFFFFFFFFFull / 10u)
                sig <<= 1, scale--;
            sig = udiv10(sig, 0);
        }
    }

    if (sig == 0) { *out = neg ? signbit : 0; return 1; }

    {
        int msb = 63, shift, E, ef;
        u64 M, bits;

        while (msb > 0 && !(sig & (1ull << msb)))
            msb--;
        shift = msb - (prec - 1);

        if (shift > 0) {
            u64 dropped = sig & ((1ull << shift) - 1);
            u64 half = 1ull << (shift - 1);
            M = sig >> shift;
            if (dropped > half || (dropped == half && (M & 1u)))
                M++;
        } else {
            M = sig << (-shift);
        }
        if (M >> prec) { M >>= 1; shift++; }        /* 进位溢出 */

        E  = (prec - 1) + scale + shift;
        ef = E + xbias;
        if (ef <= 0)
            bits = 0;                                /* 下溢 -> 0 */
        else if (ef >= (1 << ebits) - 1)
            bits = (u64)((1 << ebits) - 1) << mbias; /* 上溢 -> inf */
        else
            bits = ((u64)ef << mbias) | (M & ((1ull << mbias) - 1));

        if (want_double)
            *out = (neg ? signbit : 0) | bits;
        else
            *out = (u32)((neg ? 0x80000000u : 0u) | (u32)bits);
        return 1;
    }
}

/* 助记符表 */
typedef struct { const char *n; u32 op; } nameop_t;

static const nameop_t g_alu[] = {
    {"add",OP_ADD},{"adc",OP_ADC},{"sub",OP_SUB},{"sbb",OP_SBB},
    {"and",OP_AND},{"or",OP_OR},{"xor",OP_XOR},{"cmp",OP_CMP},{"test",OP_TEST},
    {"shl",OP_SHL},{"shr",OP_SHR},{"sar",OP_SAR},
    {"rol",OP_ROL},{"ror",OP_ROR},
    {"mul",OP_MUL},{"mulu",OP_MULU},{"div",OP_DIV},{"divu",OP_DIVU},{"rem",OP_REM},
    {"imul",OP_MUL},{"idiv",OP_DIV},          /* v4 别名：有符号乘/除 */
    {0,0}
};
static const nameop_t g_float[] = {
    {"fadd",OP_FADD},{"fsub",OP_FSUB},{"fmul",OP_FMUL},{"fdiv",OP_FDIV},
    {"fcmp",OP_FCMP},
    {0,0}
};
static const nameop_t g_un[] = {
    {"not",OP_NOT},{"neg",OP_NEG},{"inc",OP_INC},{"dec",OP_DEC},
    {"fneg",OP_FNEG},{"fabs",OP_FABS},
    {0,0}
};

static u32 lookup(const char *n, const nameop_t *T)
{
    int i;
    for (i = 0; T[i].n; i++)
        if (ieq(T[i].n, n))
            return T[i].op;
    return 0xFFFFFFFFu;
}

static void do_line(char *line)
{
    char *p = line;
    char *mn;
    char ops_buf[256];
    char raw_ops[256];          /* split_ops 会把逗号改成 NUL，数据伪指令要原始串 */
    char *ops[4];
    int nops;
    u32 op;
    int i;

    /* v4 列表参数： mn {src, ...} [-> ret] */
    int   braced = 0;
    char *srcs[8];
    int   nsrc = 0;
    char *ret_tok = 0;

    /* 注释 */
    {
        char *c = line;
        while (*c) {
            if (*c == ';' || *c == '#') { *c = 0; break; }
            if (*c == '"') {                    /* 跳过字符串里的 # ; */
                c++;
                while (*c && *c != '"') {
                    if (*c == '\\' && c[1]) c++;
                    c++;
                }
            }
            if (!*c) break;
            c++;
        }
    }

    /* 一行可能只有标签（可以连续多个） */
    for (;;) {
        char *t = trim(p);
        char *q;
        p = t;
        if (!*p)
            return;
        if (!is_alp(*p))
            break;
        q = p;
        while (*q && (is_alp(*q) || is_dig(*q) || *q == '.'))
            q++;
        if (*q != ':')
            break;
        {
            char name[64];
            int k = (int)(q - p);
            if (k > 63)
                k = 63;
            for (i = 0; i < k; i++)
                name[i] = p[i];
            name[k] = 0;
            sym_add(name, *g_cur, g_sec);
        }
        p = q + 1;
    }

    p = trim(p);
    if (!*p)
        return;

    /* loop{...} 语法糖：助记符与 '{' 之间可以没有空格 */
    if (lc(p[0]) == 'l' && lc(p[1]) == 'o' && lc(p[2]) == 'o' && lc(p[3]) == 'p') {
        char *q = p + 4;
        while (*q && is_sp(*q))
            q++;
        if (*q == '{') {
            do_loop(q);
            return;
        }
    }

    /* mnemonic */
    {
        int k = 0;
        while (p[k] && !is_sp(p[k]) && k < 31)
            k++;
        {
            static char mbuf[32];
            for (i = 0; i < k; i++)
                mbuf[i] = p[i];
            mbuf[k] = 0;
            mn = mbuf;
        }
        p += k;
    }

    {
        int k = 0;
        while (*p && k < 255)
            ops_buf[k++] = *p++;
        ops_buf[k] = 0;
        for (i = 0; i <= k; i++)
            raw_ops[i] = ops_buf[i];
    }
    nops = split_ops(ops_buf, ops, 4);

    /* ---- 伪指令 ---- */
    if (ieq(mn, "section") || ieq(mn, "segment")) {
        if (nops < 1) { asm_err("section: missing name"); return; }
        set_section(ops[0]);
        return;
    }
    if (ieq(mn, "db")) { emit_dstr(raw_ops); return; }
    if (ieq(mn, "dw") || ieq(mn, "dd") || ieq(mn, "dq")) { emit_data(mn, raw_ops); return; }
    if (ieq(mn, "resb") || ieq(mn, "resw") || ieq(mn, "resd") || ieq(mn, "resq")) {
        int esz = ieq(mn, "resw") ? 2 : ieq(mn, "resd") ? 4 : ieq(mn, "resq") ? 8 : 1;
        emit_res(ops_buf, esz);
        return;
    }
    if (ieq(mn, "var-i") || ieq(mn, "var-f") || ieq(mn, "var-p")) {
        do_var(mn, raw_ops);
        return;
    }

    /* ---- 方法定义：method name[(...)] [-> T] { ... } ---- */
    if (ieq(mn, "method") || ieq(mn, "function") || ieq(mn, "fn")) {
        char *q = trim(raw_ops);
        char nm[64];
        int k = 0;

        if (g_sec != SEC_TEXT) { asm_err("method: only allowed in .text"); return; }
        if (g_in_method) { asm_err("method: nested method not allowed"); return; }
        if (!is_alp(*q)) { asm_err("method: missing name"); return; }
        /* 参数表 / 返回值暂不解析，只取方法名 */
        while (*q && (is_alp(*q) || is_dig(*q) || *q == '.') && k < 63)
            nm[k++] = *q++;
        nm[k] = 0;
        /* 名字后只能是空白 / '(' / '{' / '->' / 行尾，否则标识符非法 */
        if (*q && !is_sp(*q) && *q != '(' && *q != '{' && *q != '-') {
            asm_err2("bad method name: ", nm);
            return;
        }
        g_in_method = 1;
        if (ieq(nm, "main")) { g_have_main = 1; g_main_off = *g_cur; }
        sym_add(nm, *g_cur, g_sec);
        return;
    }
    if (mn[0] == '}' && mn[1] == 0) {
        if (!g_in_method) { asm_err("unexpected '}'"); return; }
        g_in_method = 0;
        return;
    }

    /* 规则：所有可执行指令都必须包裹在方法内 */
    if (!g_in_method) {
        asm_err2("code outside method (wrap it in 'method main { ... }'): ", mn);
        return;
    }

    /* ---- v4：解析 {源...} 与 -> 目标（raw_ops 仅此用途） ---- */
    {
        char *r = trim(raw_ops);
        char *arrow = find_arrow(r);
        if (arrow) {
            *arrow = 0;
            ret_tok = trim(arrow + 2);
            r = trim(r);
        }
        if (*r == '{') {
            char *close = r + strlen(r);
            while (close > r && close[-1] != '}')
                close--;
            if (close > r) {
                close[-1] = 0;
                braced = 1;
                nsrc = split_ops(r + 1, srcs, 8);
            }
        }
    }

    /* ---- 无操作数 ---- */
    if (ieq(mn, "nop")) { e_b(OP_NOP); return; }
    if (ieq(mn, "hlt")) { e_b(OP_HLT); return; }
    if (ieq(mn, "ret")) { e_b(OP_S_RET); e_b(0); return; }
    if (ieq(mn, "endloop")) { do_endloop(); return; }

    /* ---- intervm / svc ---- */
    if (ieq(mn, "intervm") || ieq(mn, "svc")) {
        if (braced) {
            /* intervm {id, a, b} -> ret ：id 是立即数，a/b 走 R0/R1 */
            opnd_t ido, a, b;
            int ha, hb;
            u32 idv = 0;
            if (nsrc < 1) { asm_err("intervm: missing id"); return; }
            parse_operand(srcs[0], &ido);
            resolve_svc_id(&ido, &idv);
            ha = nsrc >= 2;
            hb = nsrc >= 3;
            if (ha) parse_operand(srcs[1], &a);
            if (hb) parse_operand(srcs[2], &b);
            if (ha && is_null_opnd(&a)) ha = 0;
            if (hb && is_null_opnd(&b)) hb = 0;
            if (ha) emit_load_reg(DWN_SCRATCH, &a);
            if (hb) emit_load_reg(DWN_SCRATCH2, &b);
            if (ha) emit_rrr(OP_MOV, 0, DWN_SCRATCH, 0, DWN_SIZE_FB);
            if (hb) emit_rrr(OP_MOV, 1, DWN_SCRATCH2, 0, DWN_SIZE_FB);
            emit_svc(idv);
            if (ret_tok && ret_tok[0]) {
                opnd_t r;
                parse_operand(ret_tok, &r);
                emit_ret_from(0, &r);
            }
            return;
        }
        {
            opnd_t a;
            u32 idv = 0;
            if (nops < 1) { asm_err("intervm: missing id"); return; }
            parse_operand(ops[0], &a);
            resolve_svc_id(&a, &idv);
            emit_svc(idv);
        }
        return;
    }

    /* ---- 变量收发 ---- */
    if (ieq(mn, "ldv")) {
        if (nops < 2) { asm_err("ldv: need dst, var"); return; }
        do_ldv(ops[0], ops[1]);
        return;
    }
    if (ieq(mn, "stv")) {
        if (nops < 2) { asm_err("stv: need var, src"); return; }
        do_stv(ops[0], ops[1]);
        return;
    }

    /* ---- v4 列表形式：j<cc> {a,b} -> 标签 / ALU / 浮点 ---- */
    if (braced) {
        /* j<cc> {a, b} -> 标签 ：融合一条 cmp */
        if (lc(mn[0]) == 'j') {
            int cc2 = cond_cc(mn + 1);
            if (cc2 >= 0 && nsrc >= 2 && ret_tok && ret_tok[0]) {
                opnd_t a, b;
                parse_operand(srcs[0], &a);
                parse_operand(srcs[1], &b);
                if (b.kind == K_IMM)
                    emit_imm(OP_CMP, (u32)a.reg,
                             b.width_set ? width_to_size(b.width) : DWN_SIZE_FB, &b);
                else if (a.kind == K_IMM) {
                    asm_err("jcc: immediate must be the second operand");
                    return;
                } else
                    emit_rrr(OP_CMP, (u32)a.reg, (u32)a.reg, (u32)b.reg, DWN_SIZE_FB);
                emit_branch(OP_JCC, cc2, ret_tok);
                return;
            }
        }

        op = lookup(mn, g_alu);
        if (op != 0xFFFFFFFFu || lookup(mn, g_float) != 0xFFFFFFFFu) {
            int isflt = (op == 0xFFFFFFFFu);
            opnd_t a, b, d;
            u32 size = isflt ? DWN_SIZE_FB : DWN_SIZE_FB;
            if (isflt) op = lookup(mn, g_float);
            if (nsrc < 2) { asm_err("need {a, b}"); return; }
            parse_operand(srcs[0], &a);
            parse_operand(srcs[1], &b);
            if (ret_tok && ret_tok[0])
                parse_operand(ret_tok, &d);
            else
                d = a;
            if (!isflt && (a.kind == K_PAIR || b.kind == K_PAIR || d.kind == K_PAIR))
                size = DWN_SIZE_EB;
            if (d.kind != K_REG && d.kind != K_PAIR) {
                asm_err("target must be a register");
                return;
            }
            if (a.kind == K_IMM) {
                asm_err("immediate must be the second operand");
                return;
            }
            if (size == DWN_SIZE_EB &&
                (op == OP_MUL || op == OP_MULU || op == OP_DIV ||
                 op == OP_DIVU || op == OP_REM)) {
                asm_err("64-bit mul/div not supported");
                return;
            }
            if (b.kind == K_IMM) {
                if (d.reg != a.reg)
                    emit_rrr(OP_MOV, (u32)d.reg, (u32)a.reg, 0, size);
                emit_imm(op, (u32)d.reg, b.width_set ? width_to_size(b.width) : size, &b);
            } else {
                emit_rrr(op, (u32)d.reg, (u32)a.reg, (u32)b.reg, size);
            }
            return;
        }
    }

    /* ---- 分支 ---- */
    if (ieq(mn, "jmp")) {
        if (nops < 1) { asm_err("jmp: missing target"); return; }
        emit_branch(OP_JMP, CC_AL, ops[0]);
        return;
    }
    if (ieq(mn, "call")) {
        if (nops < 1) { asm_err("call: missing target"); return; }
        emit_branch(OP_CALL, CC_AL, ops[0]);
        return;
    }
    if (mn[0] == 'j' || mn[0] == 'J') {
        int cc;
        if (ieq(mn, "jcc") && nops >= 2) {
            cc = cond_cc(ops[0]);
            if (cc < 0) { asm_err2("bad condition: ", ops[0]); return; }
            emit_branch(OP_JCC, cc, ops[1]);
            return;
        }
        cc = cond_cc(mn + 1);
        if (cc >= 0) {
            if (nops < 1) { asm_err("jcc: missing target"); return; }
            emit_branch(OP_JCC, cc, ops[0]);
            return;
        }
    }

    /* ---- 传送 ---- */
    if (ieq(mn, "ldi")) {
        opnd_t d, s;
        u32 size;
        if (nops < 2) { asm_err("ldi: need dst, imm"); return; }
        parse_operand(ops[0], &d);
        parse_operand(ops[1], &s);
        if (d.kind != K_REG && d.kind != K_PAIR) { asm_err("ldi: dst must be a register"); return; }
        size = (d.kind == K_PAIR) ? DWN_SIZE_EB
             : (s.width_set ? width_to_size(s.width) : width_to_size(d.width));
        emit_imm(OP_LDI, (u32)d.reg, size, &s);
        return;
    }
    if (ieq(mn, "mov")) {
        opnd_t d, s;
        if (nops < 2) { asm_err("mov: need dst, src"); return; }
        parse_operand(ops[0], &d);
        parse_operand(ops[1], &s);
        if (d.kind == K_PAIR || s.kind == K_PAIR) {
            emit_rrr(OP_MOV, (u32)d.reg, (u32)s.reg, 0, DWN_SIZE_EB);
        } else {
            e_b(OP_S_MOV);
            e_b((((u32)d.reg & 0xF) << 4) | ((u32)s.reg & 0xF));
        }
        return;
    }
    if (ieq(mn, "ld")) {
        opnd_t d, m;
        if (nops < 2) { asm_err("ld: need dst, [mem]"); return; }
        parse_operand(ops[0], &d);
        parse_operand(ops[1], &m);
        if (m.kind != K_MEM) { asm_err("ld: src must be [mem]"); return; }
        {
            u32 size = (d.kind == K_PAIR) ? DWN_SIZE_EB : width_to_size(d.width);
            mem_apply_scale(&m);
            emit_mem(OP_LD, (u32)d.reg, size, &m, 0);
        }
        return;
    }
    if (ieq(mn, "st")) {
        opnd_t m, s;
        if (nops < 2) { asm_err("st: need [mem], src"); return; }
        parse_operand(ops[0], &m);
        parse_operand(ops[1], &s);
        if (m.kind != K_MEM) { asm_err("st: dst must be [mem]"); return; }
        {
            u32 size = (s.kind == K_PAIR) ? DWN_SIZE_EB : width_to_size(s.width);
            mem_apply_scale(&m);
            emit_mem(OP_ST, (u32)s.reg, size, &m, 1);
        }
        return;
    }

    /* ---- 一元 ---- */
    op = lookup(mn, g_un);
    if (op != 0xFFFFFFFFu) {
        opnd_t a, b;
        int two = 0;
        if (nops < 1) { asm_err("missing operand"); return; }
        parse_operand(ops[0], &a);
        if (mn[0] == 'f' || mn[0] == 'F') {
            if (nops >= 2) { parse_operand(ops[1], &b); two = 1; }
            emit_rrr(op, (u32)a.reg, (u32)(two ? b.reg : a.reg), 0, DWN_SIZE_FB);
            return;
        }
        if (a.kind == K_PAIR) {
            emit_rrr(op, (u32)a.reg, (u32)a.reg, (u32)a.reg, DWN_SIZE_EB);
            return;
        }
        e_b(short_of(op));
        e_b((((u32)a.reg & 0xF) << 4));
        return;
    }

    /* ---- push / pop ---- */
    if (ieq(mn, "push") || ieq(mn, "pop")) {
        opnd_t a;
        u32 bop = ieq(mn, "push") ? OP_PUSH : OP_POP;
        if (nops < 1) { asm_err("missing operand"); return; }
        parse_operand(ops[0], &a);
        if (a.kind == K_PAIR) {
            emit_rrr(bop, (u32)a.reg, 0, 0, DWN_SIZE_EB);
        } else {
            e_b(short_of(bop));
            e_b((((u32)a.reg & 0xF) << 4));
        }
        return;
    }

    /* ---- 浮点二元 ---- */
    op = lookup(mn, g_float);
    if (op != 0xFFFFFFFFu) {
        opnd_t d, a, b;
        if (nops < 2) { asm_err("float op: need operands"); return; }
        parse_operand(ops[0], &d);
        parse_operand(ops[1], &a);
        if (nops >= 3) {
            parse_operand(ops[2], &b);
            if (b.kind == K_IMM) {
                if (a.reg != d.reg) { asm_err("imm form needs dst==a"); return; }
                emit_imm(op, (u32)d.reg, DWN_SIZE_FB, &b);
            } else {
                emit_rrr(op, (u32)d.reg, (u32)a.reg, (u32)b.reg, DWN_SIZE_FB);
            }
        } else {
            if (a.kind == K_IMM) {
                emit_imm(op, (u32)d.reg, DWN_SIZE_FB, &a);
            } else {
                emit_rrr(op, (u32)d.reg, (u32)d.reg, (u32)a.reg, DWN_SIZE_FB);
            }
        }
        return;
    }

    /* ---- 整数 ALU ---- */
    op = lookup(mn, g_alu);
    if (op != 0xFFFFFFFFu) {
        opnd_t d, a, b;
        u32 size;
        int is64;
        if (nops < 2) { asm_err("alu: need operands"); return; }
        parse_operand(ops[0], &d);
        parse_operand(ops[1], &a);
        is64 = (d.kind == K_PAIR) || (a.kind == K_PAIR);
        if (nops >= 3) {
            parse_operand(ops[2], &b);
            if (b.kind == K_PAIR) is64 = 1;
        }
        size = is64 ? DWN_SIZE_EB : DWN_SIZE_FB;
        if (!is64 && a.kind == K_IMM) {
            size = a.width_set ? width_to_size(a.width) : DWN_SIZE_FB;
            emit_imm(op, (u32)d.reg, size, &a);
            return;
        }
        if (is64) {
            /* 乘除移位在 VM 里只做了加/减/逻辑/移位，乘除不支持 64 位 */
            if (op == OP_MUL || op == OP_MULU || op == OP_DIV || op == OP_DIVU ||
                op == OP_REM) {
                asm_err("64-bit mul/div not supported");
                return;
            }
        }
        if (nops >= 3) {
            if (b.kind == K_IMM) {
                if (a.kind != K_REG && a.kind != K_PAIR) { asm_err("bad operand"); return; }
                if (a.reg != d.reg) { asm_err("imm form needs dst==a"); return; }
                emit_imm(op, (u32)d.reg, b.width_set ? width_to_size(b.width) : size, &b);
                return;
            }
            emit_rrr(op, (u32)d.reg, (u32)a.reg, (u32)b.reg, size);
            return;
        }
        /* 二操作数：op dst, b  ->  op dst, dst, b */
        if (a.kind == K_IMM) {
            emit_imm(op, (u32)d.reg, a.width_set ? width_to_size(a.width) : size, &a);
        } else {
            emit_rrr(op, (u32)d.reg, (u32)d.reg, (u32)a.reg, size);
        }
        return;
    }

    asm_err2("unknown instruction: ", mn);
}

/* ---------------- 两遍扫描 ---------------- */

static void do_pass(int pass)
{
    const char *s = (const char *)g_src;
    char line[512];

    g_pass = pass;
    g_line = 0;
    g_sec = SEC_TEXT;
    g_text_pos = 0;
    g_data_pos = 0;
    g_cur = &g_text_pos;
    g_in_method = 0;
    g_have_main = 0;
    g_main_off = 0;
    g_loop_depth = 0;
    g_loop_next = 0;
    if (pass == 1) {
        g_nvar = 0;
        g_heap_size = 0;
    }

    while (*s) {
        int n = 0;
        while (*s && *s != '\n' && n < 511)
            line[n++] = *s++;
        if (*s == '\n')
            s++;
        line[n] = 0;
        g_line++;
        {
            int i;
            for (i = 0; line[i] && i < 127; i++)
                g_cur_src[i] = line[i];
            g_cur_src[i] = 0;
        }
        do_line(line);
    }

    close_section();

    if (pass == 1) {
        if (g_in_method)
            asm_err("unclosed method (missing '}')");
        if (!g_have_main)
            asm_err("no 'main' method (execution starts at main)");
    }
}

/* ---------------- 入口 ---------------- */

static char *first_word(char **pp)
{
    char *s = *pp;
    char *b;
    while (*s && is_sp(*s))
        s++;
    if (!*s) { *pp = s; return 0; }
    b = s;
    while (*s && !is_sp(*s))
        s++;
    if (*s)
        *s++ = 0;
    *pp = s;
    return b;
}

__attribute__((section(".text._start"), used, noreturn))
void dasm_entry(void)
{
    char *args, *src, *dst, *p;
    int n;
    u32 image_size, i;
    u32 chk;

    __asm__ volatile("mov %%eax, %0" : "=r"(args));
    args = (char *)args;

    if (!args || !args[0]) {
        sys_ioprintln("dasm: usage: dasm <src.dasm> <out.dwn>");
        sys_exit(1);
    }
    p = args;
    src = first_word(&p);
    dst = first_word(&p);
    if (!src || !dst) {
        sys_ioprintln("dasm: usage: dasm <src.dasm> <out.dwn>");
        sys_exit(1);
    }

    n = sys_fs_read(src, g_src, SRC_MAX - 1);
    if (n < 0) {
        char b[160];
        sprintf(b, "dasm: cannot open %s", src);
        sys_ioprintln(b);
        sys_exit(2);
    }
    if (n == 0) {
        char b[160];
        sprintf(b, "dasm: %s is empty", src);
        sys_ioprintln(b);
        sys_exit(3);
    }
    g_src[n] = 0;

    g_err = 0;
    g_nsym = 0;
    g_err_msg[0] = 0;
    g_err_msg[1] = 0;
    g_err_src[0] = 0;

    do_pass(1);

    g_data_base = (g_text_len + 3u) & ~3u;
    image_size = (g_data_len == 0) ? g_text_len : (g_data_base + g_data_len);

    /* 变量堆紧跟在镜像之后（VM 内存零初始化，正好当 .bss 用） */
    g_heap_base = (image_size + 3u) & ~3u;

    if (g_heap_size && g_heap_base + g_heap_size > DWN_MEM_SIZE) {
        char buf[160];
        sprintf(buf, "dasm: heap too large (vars %u + image %u > %u)",
                g_heap_size, image_size, (u32)DWN_MEM_SIZE);
        sys_ioprintln(buf);
        sys_exit(3);
    }

    memset(g_out + DWN_HDR_SIZE, 0, DWN_MEM_SIZE + 8);

    do_pass(2);

    if (g_err || g_text_len == 0 || image_size == 0 || image_size > DWN_MEM_SIZE) {
        char buf[220];
        if (g_err) {
            sprintf(buf, "dasm: error on line %d: %s", g_err_line, g_err_msg);
            sys_ioprintln(buf);
            if (g_err_src[0]) {
                sprintf(buf, "  > %s", g_err_src);
                sys_ioprintln(buf);
            }
        } else if (g_text_len == 0) {
            sys_ioprintln("dasm: no .text code (program needs at least one instruction)");
        } else {
            sprintf(buf, "dasm: image too large (%u bytes, max %u)", image_size, DWN_MEM_SIZE);
            sys_ioprintln(buf);
        }
        sys_exit(3);
    }

    /* 头 */
    memset(g_out, 0, DWN_HDR_SIZE);
    g_out[0] = 'D'; g_out[1] = 'W'; g_out[2] = 'N'; g_out[3] = 0;
    g_out[4] = DWN_VERSION & 0xFF;
    g_out[5] = (DWN_VERSION >> 8) & 0xFF;
    g_out[6] = DWN_HDR_SIZE & 0xFF;
    g_out[7] = (DWN_HDR_SIZE >> 8) & 0xFF;
    /* entry = main 方法在 .text 内的偏移（执行从 main 开始） */
    g_out[8]  = g_main_off & 0xFF;
    g_out[9]  = (g_main_off >> 8) & 0xFF;
    g_out[10] = (g_main_off >> 16) & 0xFF;
    g_out[11] = (g_main_off >> 24) & 0xFF;
    g_out[12] = image_size & 0xFF;
    g_out[13] = (image_size >> 8) & 0xFF;
    g_out[14] = (image_size >> 16) & 0xFF;
    g_out[15] = (image_size >> 24) & 0xFF;
    g_out[16] = g_data_base & 0xFF;
    g_out[17] = (g_data_base >> 8) & 0xFF;
    g_out[18] = (g_data_base >> 16) & 0xFF;
    g_out[19] = (g_data_base >> 24) & 0xFF;
    /* bss_size(20-23) = 0, flags(24-27) = 0 */

    /* checksum：checksum 字段按 0 计 */
    chk = 2166136261u;
    for (i = 0; i < DWN_HDR_SIZE; i++) {
        u8 b = (i >= 28 && i < 32) ? 0u : g_out[i];
        chk ^= b;
        chk *= 16777619u;
    }
    for (i = 0; i < image_size; i++) {
        chk ^= g_out[DWN_HDR_SIZE + i];
        chk *= 16777619u;
    }
    g_out[28] = chk & 0xFF;
    g_out[29] = (chk >> 8) & 0xFF;
    g_out[30] = (chk >> 16) & 0xFF;
    g_out[31] = (chk >> 24) & 0xFF;

    n = sys_fs_write(dst, g_out, DWN_HDR_SIZE + image_size);
    if (n != (int)(DWN_HDR_SIZE + image_size)) {
        sys_ioprintln("dasm: cannot write output");
        sys_exit(4);
    }

    sys_ioprintln("dasm: ok");
    sys_exit(0);

    for (;;)
        ;
}
