/*
 * limasm - a tiny NASM-like assembler for PoserOS
 *
 * Produces .exc executables (PEXC format). Runs as a ring3 .exc program,
 * invoked by the shell as:  limasm <src.asm> <out.exc>
 *
 * Supported:
 *   mov / lea
 *   add sub adc sbb and or xor cmp test       (four-function + logic)
 *   inc dec not neg mul imul div idiv
 *   shl sal shr sar rol ror rcl rcr           (bit ops)
 *   push pop jmp call ret nop
 *   jcc (jz jnz je jne jg jge jl jle ja jae jb jbe js jns jo jno jp jnp)
 *   lodsb lodsw lodsd                         (no rep)
 *   int imm8                                  (int 0x80 syscalls)
 *   db dw dd resb resw resd
 *
 * Addressing is 32-bit: [disp] [base] [base+disp] [base+index*scale+disp]
 */

#include "API-LIB/Pstd/pstd_kapi.h"
#include "API-LIB/Pstd/pstd_string.h"
#include "API-LIB/Pstd/pstd_exc.h"

#define ORIGIN    0x04000000u
#define CODE_OFF  PEXC_HDR_SIZE
#define MAX_CODE  40000u          /* .text 上限 */
#define MAX_DATA  40000u          /* .data 上限 */
#define MAX_IMAGE (MAX_CODE + MAX_DATA)
#define MAX_SRC   40000u
#define SYM_MAX   512

/* 段选择：代码从 .text 第一条指令开始执行（entry = ORIGIN）；
 * .data 紧跟在 .text 之后（4 字节对齐），两者按 VA 顺序拼进同一个镜像。
 * 全部放 .data 段，objcopy 才会把它们收进扁平二进制里。 */
#define SEC_TEXT  0
#define SEC_DATA  1

static u8  g_out[CODE_OFF + MAX_IMAGE + 4] __attribute__((section(".data"))) = {1};
static u8  g_src[MAX_SRC]                  __attribute__((section(".data"))) = {1};

typedef struct {
    char name[40];
    u32  addr;
    int  def;
    int  sec;      /* 符号所属段：SEC_TEXT / SEC_DATA */
} sym_t;

static sym_t g_syms[SYM_MAX] __attribute__((section(".data"))) = {{1}};
static int   g_nsym;
static int   g_pass;
static int   g_sec;            /* 当前段 */
static u32   g_addr;           /* 当前段内的 VA 计数 */
static u32   g_text_addr;      /* .text 计数（切段时保存） */
static u32   g_data_addr;      /* .data 计数（切段时保存） */
static u32   g_data_base;      /* .data 的 VA 基址（两遍之间算出来） */
static u32   g_text_size;      /* 第一遍得到的 .text 大小 */
static u32   g_data_size;      /* 第一遍得到的 .data 大小 */
static int   g_overflow;       /* 输出超出一节可容纳范围 */
static int   g_err;

/* 错误诊断：记下第一处错误的行号、原因和对应的源码行 */
static int   g_line;          /* 当前行号，从 1 开始 */
static int   g_err_seen;
static int   g_err_line;
static char  g_err_msg[80]  __attribute__((section(".data"))) = {1};
static char  g_err_src[128] __attribute__((section(".data"))) = {1};
static char  g_cur_src[128] __attribute__((section(".data"))) = {1};

/* 记录第一处错误（后续错误只置标志，不覆盖第一条） */
static void asm_err(const char *msg)
{
    int i;

    g_err = 1;
    if (g_err_seen)
        return;
    g_err_seen = 1;
    g_err_line = g_line;
    for (i = 0; msg[i] && i < 79; i++)
        g_err_msg[i] = msg[i];
    g_err_msg[i] = 0;
    for (i = 0; g_cur_src[i] && i < 127; i++)
        g_err_src[i] = g_cur_src[i];
    g_err_src[i] = 0;
}

/* 带一个字符串参数的错误，如 "undefined symbol: " + name */
static void asm_err2(const char *a, const char *b)
{
    char m[80];
    int i = 0, j;

    while (a[i] && i < 60) {
        m[i] = a[i];
        i++;
    }
    for (j = 0; b && b[j] && i < 79; j++)
        m[i++] = b[j];
    m[i] = 0;
    asm_err(m);
}

/* ---------------- small string helpers ---------------- */

static int is_sp(int c)    { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_dig(int c)   { return c >= '0' && c <= '9'; }
static int is_alp(int c)   { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '@'; }
static int is_an(int c)    { return is_alp(c) || is_dig(c); }
static int lc(int c)       { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

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
    e = s;
    while (*e)
        e++;
    while (e > s && is_sp(e[-1]))
        *--e = 0;
    return s;
}

/* ---------------- symbol table ---------------- */

static void sym_add(const char *name, u32 addr)
{
    int i;
    char *d;

    for (i = 0; i < g_nsym; i++) {
        if (ieq(g_syms[i].name, name)) {
            if (g_pass == 1)
                g_syms[i].addr = addr;
            return;
        }
    }
    if (g_nsym >= SYM_MAX) {
        asm_err("too many symbols");
        return;
    }
    d = g_syms[g_nsym].name;
    for (i = 0; name[i] && i < 39; i++)
        d[i] = name[i];
    d[i]   = 0;
    g_syms[g_nsym].addr = addr;
    g_syms[g_nsym].def  = 1;
    g_syms[g_nsym].sec  = g_sec;
    g_nsym++;
}

static u32 sym_val(const char *name)
{
    int i;

    if (!name || !name[0])
        return 0;
    for (i = 0; i < g_nsym; i++)
        if (ieq(g_syms[i].name, name))
            return g_syms[i].addr;
    if (g_pass == 2)
        asm_err2("undefined symbol: ", name);   /* 标签未知只在第二遍报错 */
    return 0;
}

/* ---------------- byte emitter ---------------- */

static void emitb(u32 b)
{
    if (g_pass == 2) {
        u32 off = g_addr - ORIGIN;
        if (off < MAX_IMAGE)
            g_out[CODE_OFF + off] = (u8)b;
        else
            g_overflow = 1;
    }
    g_addr++;
}

static void emit32(u32 v)
{
    emitb(v & 0xFF);
    emitb((v >> 8) & 0xFF);
    emitb((v >> 16) & 0xFF);
    emitb((v >> 24) & 0xFF);
}

static int fits8(i32 v) { return v >= -128 && v <= 127; }

/* ---------------- registers ---------------- */

typedef struct { const char *n; int code; int size; } rtab_t;

static const rtab_t g_regs[] = {
    {"al",0,1},{"cl",1,1},{"dl",2,1},{"bl",3,1},
    {"ah",4,1},{"ch",5,1},{"dh",6,1},{"bh",7,1},
    {"ax",0,2},{"cx",1,2},{"dx",2,2},{"bx",3,2},
    {"sp",4,2},{"bp",5,2},{"si",6,2},{"di",7,2},
    {"eax",0,4},{"ecx",1,4},{"edx",2,4},{"ebx",3,4},
    {"esp",4,4},{"ebp",5,4},{"esi",6,4},{"edi",7,4},
    {0,0,0}
};

static int reg_idx(const char *s)
{
    int i;

    for (i = 0; g_regs[i].n; i++)
        if (ieq(s, g_regs[i].n))
            return i;
    return -1;
}

static int reg3(int code) { return code & 7; }

/* ---------------- operand ---------------- */

#define O_NONE 0
#define O_REG  1
#define O_IMM  2
#define O_MEM  3

typedef struct {
    int  type;
    int  size;      /* 1,2,4 or 0 when unknown */
    int  reg;       /* register code for O_REG */
    int  base;      /* base register code or -1 */
    int  index;     /* index register code or -1 */
    int  scale;     /* 1,2,4,8 */
    int  hassym;    /* symbolic constant present */
    i32  disp;
    i32  imm;
    char sym[40];
} op_t;

static i32 parse_num(const char **pps, int *ok)
{
    const char *s = *pps;
    i32 v = 0;
    int base = 10, any = 0;

    *ok = 1;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
        while ((*s >= '0' && *s <= '9') || (lc(*s) >= 'a' && lc(*s) <= 'f')) {
            int d = is_dig(*s) ? *s - '0' : lc(*s) - 'a' + 10;
            v = v * base + d;
            s++;
            any = 1;
        }
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        s += 2;
        while (*s == '0' || *s == '1') {
            v = v * 2 + (*s - '0');
            s++;
            any = 1;
        }
    } else if (*s == '\'') {
        s++;
        v = *s ? *s : 0;
        if (*s)
            s++;
        if (*s == '\'')
            s++;
        any = 1;
    } else {
        while (is_dig(*s)) {
            v = v * 10 + (*s - '0');
            s++;
            any = 1;
        }
    }

    if (!any)
        *ok = 0;
    *pps = s;
    return v;
}

/* evaluate "label + 12" / "0x10" / "$" etc. */
static void eval_expr(const char *s, i32 *val, char *sym, int *known)
{
    int sign = 1;

    *val = 0;
    *known = 1;
    sym[0] = 0;

    while (*s) {
        while (is_sp(*s))
            s++;
        if (*s == '+') { sign = 1;  s++; continue; }
        if (*s == '-') { sign = -1; s++; continue; }
        if (!*s)
            break;

        if (*s == '$') {
            *val += sign * (i32)g_addr;
            s++;
        } else if (is_dig(*s) || *s == '\'') {
            int ok;
            i32 n = parse_num(&s, &ok);
            if (!ok) { *known = 0; break; }
            *val += sign * n;
        } else if (is_alp(*s)) {
            int i = 0;
            while (is_an(*s) && i < 39)
                sym[i++] = *s++;
            sym[i] = 0;
            *known = 0;
        } else {
            *known = 0;
            break;
        }
        sign = 1;
    }
}

static void parse_mem(const char *in, op_t *o)
{
    const char *p = in;
    char term[64];

    o->type  = O_MEM;
    o->base  = -1;
    o->index = -1;
    o->scale = 1;

    while (*p) {
        int sign = 1, n = 0, r;
        char *star;
        i32 v;
        char s2[40];
        int kn;

        while (is_sp(*p))
            p++;
        if (*p == '+') { sign = 1;  p++; }
        else if (*p == '-') { sign = -1; p++; }
        while (is_sp(*p))
            p++;

        while (*p && *p != '+' && *p != '-' && !is_sp(*p) && n < 63)
            term[n++] = *p++;
        term[n] = 0;
        if (!n)
            break;

        if (is_sp(*p)) {
            while (is_sp(*p))
                p++;
            if (*p != '+' && *p != '-')
                continue;
        }

        star = term;
        while (*star && *star != '*')
            star++;

        if (*star == '*') {
            char *a = term, *b;
            int r1, r2;
            *star = 0;
            b = star + 1;
            a = trim(a);
            b = trim(b);
            r1 = reg_idx(a);
            r2 = reg_idx(b);
            if (r1 >= 0) {
                o->index = g_regs[r1].code;
                o->scale = (int)parse_num((const char **)&b, &kn);
            } else if (r2 >= 0) {
                const char *q = a;
                o->index = g_regs[r2].code;
                o->scale = (int)parse_num(&q, &kn);
            } else {
                asm_err("bad scaled index register");
            }
            continue;
        }

        r = reg_idx(term);
        if (r >= 0) {
            if (o->base < 0)
                o->base = g_regs[r].code;
            else if (o->index < 0) {
                o->index = g_regs[r].code;
                o->scale = 1;
            } else
                asm_err("too many registers in memory operand");
            continue;
        }

        eval_expr(term, &v, s2, &kn);
        o->disp += sign * v;
        if (!kn) {
            int i;
            for (i = 0; s2[i] && i < 39; i++)
                o->sym[i] = s2[i];
            o->sym[i] = 0;
            o->hassym = 1;
        }
    }

    if (o->index == 4)   /* esp cannot be an index */
        asm_err("esp cannot be an index register");
}

/* if operand starts with "byte"/"word"/"dword" prefix, consume it */
static int take_size_prefix(char **pp)
{
    char *p = *pp;
    static const struct { const char *w; int sz; } tab[] = {
        {"byte", 1}, {"word", 2}, {"dword", 4}, {0, 0}
    };
    int i;

    for (i = 0; tab[i].w; i++) {
        int j = 0;
        while (tab[i].w[j] && lc(p[j]) == tab[i].w[j])
            j++;
        if (tab[i].w[j])
            continue;
        if (!is_sp(p[j]) && p[j] != '[')
            continue;
        *pp = trim(p + j);
        return tab[i].sz;
    }
    return 0;
}

static void parse_op(char *s, op_t *o)
{
    char *p;
    int r, kn;

    memset(o, 0, sizeof(*o));
    o->base  = -1;
    o->index = -1;
    o->scale = 1;

    p = trim(s);

    o->size = take_size_prefix(&p);

    if (*p == '[') {
        char *e = p + 1;
        int depth = 1;
        while (*e && depth) {
            if (*e == '[') depth++;
            else if (*e == ']') depth--;
            if (depth) e++;
        }
        if (*e == ']')
            *e = 0;
        parse_mem(p + 1, o);
        return;
    }

    r = reg_idx(p);
    if (r >= 0) {
        o->type = O_REG;
        o->reg  = g_regs[r].code;
        o->size = g_regs[r].size;
        return;
    }

    o->type = O_IMM;
    eval_expr(p, &o->imm, o->sym, &kn);
    o->hassym = !kn;
    if (!o->size)
        o->size = 4;
}

static u32 res_imm(op_t *o)
{
    u32 v = (u32)o->imm;
    if (o->hassym)
        v += sym_val(o->sym);
    return v;
}

static u32 res_disp(op_t *o)
{
    u32 v = (u32)o->disp;
    if (o->hassym)
        v += sym_val(o->sym);
    return v;
}

static void emit_imm(op_t *o, int nbytes)
{
    u32 v = res_imm(o);
    int i;
    for (i = 0; i < nbytes; i++)
        emitb((v >> (8 * i)) & 0xFF);
}

static void emit_disp(op_t *o, int mod)
{
    if (mod == 1)
        emitb((u32)(o->disp) & 0xFF);
    else if (mod == 2)
        emit32(res_disp(o));
}

static int scale_bits(int s)
{
    if (s == 2) return 1;
    if (s == 4) return 2;
    if (s == 8) return 3;
    return 0;
}

static void emit_mem(op_t *o, int regf)
{
    int base = o->base, index = o->index, mod;

    if (base < 0) {
        if (index < 0) {
            emitb(0x05 | ((regf & 7) << 3));
            emit32(res_disp(o));
            return;
        }
        emitb(((regf & 7) << 3) | 4);
        emitb((scale_bits(o->scale) << 6) | ((index & 7) << 3) | 5);
        emit32(res_disp(o));
        return;
    }

    if (index >= 0 || (base & 7) == 4) {
        int b = base & 7;
        if (o->hassym || (b == 5 && !fits8(o->disp)))
            mod = 2;
        else if (b == 5)
            mod = 1;
        else if (o->disp == 0)
            mod = 0;
        else if (fits8(o->disp))
            mod = 1;
        else
            mod = 2;

        emitb((mod << 6) | ((regf & 7) << 3) | 4);
        emitb((scale_bits(o->scale) << 6) | (((index < 0 ? 4 : index) & 7) << 3) | b);
        emit_disp(o, mod);
        return;
    }

    {
        int b = base & 7;
        if (o->hassym || (b == 5 && !fits8(o->disp)))
            mod = 2;
        else if (b == 5)
            mod = 1;
        else if (o->disp == 0)
            mod = 0;
        else if (fits8(o->disp))
            mod = 1;
        else
            mod = 2;

        emitb((mod << 6) | ((regf & 7) << 3) | b);
        emit_disp(o, mod);
    }
}

static void emit_rm(op_t *o, int regf)
{
    if (o->type == O_REG)
        emitb(0xC0 | ((regf & 7) << 3) | reg3(o->reg));
    else
        emit_mem(o, regf);
}

static int op_size(op_t *a, op_t *b)
{
    if (a && a->size)
        return a->size;
    if (b && b->size)
        return b->size;
    return 4;
}

/* ---------------- operand splitting ---------------- */

static int split_ops(char *s, char *out[], int max)
{
    int n = 0, depth = 0;
    char q = 0;
    char *start = s;

    while (*s && n < max) {
        if (q) {
            if (*s == q)
                q = 0;
        } else if (*s == '"' || *s == '\'') {
            q = *s;
        } else if (*s == '[') {
            depth++;
        } else if (*s == ']') {
            if (depth)
                depth--;
        } else if (*s == ',' && depth == 0) {
            *s = 0;
            out[n++] = trim(start);
            start = s + 1;
        }
        s++;
    }
    if (n < max && *trim(start))
        out[n++] = trim(start);
    return n;
}

/* ---------------- instruction encoding ---------------- */

static int group1_digit(const char *m)
{
    if (ieq(m, "add")) return 0;
    if (ieq(m, "or"))  return 1;
    if (ieq(m, "adc")) return 2;
    if (ieq(m, "sbb")) return 3;
    if (ieq(m, "and")) return 4;
    if (ieq(m, "sub")) return 5;
    if (ieq(m, "xor")) return 6;
    if (ieq(m, "cmp")) return 7;
    return -1;
}

static int group1_base(int d)
{
    static const int b[8] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38};
    return b[d];
}

static int shift_digit(const char *m)
{
    if (ieq(m, "rol")) return 0;
    if (ieq(m, "ror")) return 1;
    if (ieq(m, "rcl")) return 2;
    if (ieq(m, "rcr")) return 3;
    if (ieq(m, "shl") || ieq(m, "sal")) return 4;
    if (ieq(m, "shr")) return 5;
    if (ieq(m, "sar")) return 7;
    return -1;
}

static int group3_digit(const char *m)
{
    if (ieq(m, "not"))  return 2;
    if (ieq(m, "neg"))  return 3;
    if (ieq(m, "mul"))  return 4;
    if (ieq(m, "imul")) return 5;
    if (ieq(m, "div"))  return 6;
    if (ieq(m, "idiv")) return 7;
    return -1;
}

static int jcc_code(const char *m)
{
    if (ieq(m, "jo"))  return 0;
    if (ieq(m, "jno")) return 1;
    if (ieq(m, "jb") || ieq(m, "jc") || ieq(m, "jnae")) return 2;
    if (ieq(m, "jae") || ieq(m, "jnb") || ieq(m, "jnc")) return 3;
    if (ieq(m, "je") || ieq(m, "jz")) return 4;
    if (ieq(m, "jne") || ieq(m, "jnz")) return 5;
    if (ieq(m, "jbe") || ieq(m, "jna")) return 6;
    if (ieq(m, "ja") || ieq(m, "jnbe")) return 7;
    if (ieq(m, "js"))  return 8;
    if (ieq(m, "jns")) return 9;
    if (ieq(m, "jp") || ieq(m, "jpe")) return 10;
    if (ieq(m, "jnp") || ieq(m, "jpo")) return 11;
    if (ieq(m, "jl") || ieq(m, "jnge")) return 12;
    if (ieq(m, "jge") || ieq(m, "jnl")) return 13;
    if (ieq(m, "jle") || ieq(m, "jng")) return 14;
    if (ieq(m, "jg") || ieq(m, "jnle")) return 15;
    return -1;
}

static void emit_rel32(op_t *tgt)
{
    u32 t, rel;
    int i;

    t   = res_imm(tgt);
    rel = t - (g_addr + 4);
    for (i = 0; i < 4; i++)
        emitb((rel >> (8 * i)) & 0xFF);
}

static void mov_insn(op_t *a, op_t *b)
{
    if (a->type == O_REG && b->type == O_IMM) {
        if (a->size == 1) {
            emitb(0xB0 + reg3(a->reg));
            emit_imm(b, 1);
        } else if (a->size == 2) {
            emitb(0x66);
            emitb(0xB8 + reg3(a->reg));
            emit_imm(b, 2);
        } else {
            emitb(0xB8 + reg3(a->reg));
            emit_imm(b, 4);
        }
    } else if (a->type == O_REG && (b->type == O_REG || b->type == O_MEM)) {
        if (a->size == 1) {
            emitb(0x8A);
        } else {
            if (a->size == 2) emitb(0x66);
            emitb(0x8B);
        }
        emit_rm(b, reg3(a->reg));
    } else if ((a->type == O_REG || a->type == O_MEM) && b->type == O_REG) {
        if (b->size == 1) {
            emitb(0x88);
        } else {
            if (b->size == 2) emitb(0x66);
            emitb(0x89);
        }
        emit_rm(a, reg3(b->reg));
    } else if ((a->type == O_REG || a->type == O_MEM) && b->type == O_IMM) {
        int sz = op_size(a, b);
        if (sz == 1) {
            emitb(0xC6);
            emit_rm(a, 0);
            emit_imm(b, 1);
        } else {
            if (sz == 2) emitb(0x66);
            emitb(0xC7);
            emit_rm(a, 0);
            emit_imm(b, sz == 2 ? 2 : 4);
        }
    } else {
        asm_err("invalid operands for mov");
    }
}

static void group1_insn(const char *mn, op_t *a, op_t *b)
{
    int d = group1_digit(mn);
    int base = group1_base(d);

    if (b->type == O_IMM) {
        int sz = op_size(a, b);
        if (sz == 1) {
            emitb(0x80);
            emit_rm(a, d);
            emit_imm(b, 1);
        } else {
            if (sz == 2) emitb(0x66);
            if (!b->hassym && fits8(b->imm)) {
                emitb(0x83);
                emit_rm(a, d);
                emitb(res_imm(b) & 0xFF);
            } else {
                emitb(0x81);
                emit_rm(a, d);
                emit_imm(b, sz == 2 ? 2 : 4);
            }
        }
    } else if (a->type == O_REG && (b->type == O_REG || b->type == O_MEM)) {
        if (a->size == 2) emitb(0x66);
        emitb(base + (a->size == 1 ? 2 : 3));
        emit_rm(b, reg3(a->reg));
    } else if (a->type == O_MEM && b->type == O_REG) {
        if (b->size == 2) emitb(0x66);
        emitb(base + (b->size == 1 ? 0 : 1));
        emit_rm(a, reg3(b->reg));
    } else {
        asm_err2("invalid operands for ", mn);
    }
}

static void test_insn(op_t *a, op_t *b)
{
    if (b->type == O_IMM) {
        int sz = op_size(a, b);
        if (sz == 1) {
            emitb(0xF6);
            emit_rm(a, 0);
            emit_imm(b, 1);
        } else {
            if (sz == 2) emitb(0x66);
            emitb(0xF7);
            emit_rm(a, 0);
            emit_imm(b, sz == 2 ? 2 : 4);
        }
    } else if ((a->type == O_REG || a->type == O_MEM) && b->type == O_REG) {
        if (b->size == 2) emitb(0x66);
        emitb(b->size == 1 ? 0x84 : 0x85);
        emit_rm(a, reg3(b->reg));
    } else if (a->type == O_REG && (b->type == O_REG || b->type == O_MEM)) {
        if (a->size == 2) emitb(0x66);
        emitb(a->size == 1 ? 0x84 : 0x85);
        emit_rm(b, reg3(a->reg));
    } else {
        asm_err("invalid operands for test");
    }
}

static void shift_insn(const char *mn, op_t *a, op_t *b)
{
    int d = shift_digit(mn);
    int sz = a->size ? a->size : 4;

    if (b && b->type == O_REG && b->size == 1 && b->reg == 1) {
        if (sz == 2) emitb(0x66);
        emitb(sz == 1 ? 0xD2 : 0xD3);
        emit_rm(a, d);
    } else if (b && b->type == O_IMM) {
        if (sz == 2) emitb(0x66);
        if (!b->hassym && b->imm == 1) {
            emitb(sz == 1 ? 0xD0 : 0xD1);
            emit_rm(a, d);
        } else {
            emitb(sz == 1 ? 0xC0 : 0xC1);
            emit_rm(a, d);
            emitb(res_imm(b) & 0xFF);
        }
    } else {
        asm_err2("invalid operands for ", mn);
    }
}

static void one_op_group3(const char *mn, op_t *a)
{
    int d = group3_digit(mn);
    int sz = a->size ? a->size : 4;

    if (sz == 1) {
        emitb(0xF6);
        emit_rm(a, d);
    } else {
        if (sz == 2) emitb(0x66);
        emitb(0xF7);
        emit_rm(a, d);
    }
}

static void incdec_insn(const char *mn, op_t *a)
{
    int d = ieq(mn, "inc") ? 0 : 1;
    int reg_form = (a->type == O_REG && (a->size == 2 || a->size == 4));

    if (reg_form) {
        if (a->size == 2) emitb(0x66);
        emitb((d ? 0x48 : 0x40) + reg3(a->reg));
    } else {
        int sz = a->size ? a->size : 4;
        if (sz == 1) {
            emitb(0xFE);
            emit_rm(a, d);
        } else {
            if (sz == 2) emitb(0x66);
            emitb(0xFF);
            emit_rm(a, d);
        }
    }
}

static void pushpop_insn(const char *mn, op_t *a)
{
    int pop = ieq(mn, "pop");

    if (a->type == O_REG) {
        if (a->size == 2) emitb(0x66);
        emitb((pop ? 0x58 : 0x50) + reg3(a->reg));
    } else if (a->type == O_IMM && !pop) {
        if (!a->hassym && fits8(a->imm)) {
            emitb(0x6A);
            emitb(res_imm(a) & 0xFF);
        } else {
            emitb(0x68);
            emit_imm(a, 4);
        }
    } else if (a->type == O_MEM) {
        int sz = a->size ? a->size : 4;
        if (sz == 2) emitb(0x66);
        emitb(pop ? 0x8F : 0xFF);
        emit_rm(a, pop ? 0 : 6);
    } else {
        asm_err2("invalid operand for ", mn);
    }
}

static void calljmp_insn(const char *mn, op_t *a)
{
    int iscall = ieq(mn, "call");
    int isjmp  = ieq(mn, "jmp");

    if (a->type == O_REG || a->type == O_MEM) {
        int sz = a->size ? a->size : 4;
        if (sz == 2) emitb(0x66);
        emitb(0xFF);
        emit_rm(a, iscall ? 2 : 4);
        return;
    }

    if (isjmp) {
        emitb(0xE9);
        emit_rel32(a);
    } else if (iscall) {
        emitb(0xE8);
        emit_rel32(a);
    }
}

static void assemble_insn(char *mn, char *rest)
{
    char *ops[3];
    int n;
    op_t a, b;

    n = split_ops(rest, ops, 3);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.base = a.index = b.base = b.index = -1;
    a.scale = b.scale = 1;

    if (n >= 1)
        parse_op(ops[0], &a);
    if (n >= 2)
        parse_op(ops[1], &b);

    if (ieq(mn, "nop")) { emitb(0x90); return; }
    if (ieq(mn, "ret")) { emitb(0xC3); return; }

    if (ieq(mn, "lodsb")) { emitb(0xAC); return; }
    if (ieq(mn, "lodsw")) { emitb(0x66); emitb(0xAD); return; }
    if (ieq(mn, "lodsd")) { emitb(0xAD); return; }

    if (ieq(mn, "int")) {
        if (n < 1 || a.type != O_IMM) { asm_err("int needs an imm8 operand"); return; }
        emitb(0xCD);
        emitb(res_imm(&a) & 0xFF);
        return;
    }

    if (ieq(mn, "mov")) { mov_insn(&a, &b); return; }
    if (ieq(mn, "lea")) {
        if (a.type != O_REG || b.type != O_MEM) { asm_err("invalid operands for lea"); return; }
        if (a.size == 2) emitb(0x66);
        emitb(0x8D);
        emit_rm(&b, reg3(a.reg));
        return;
    }

    if (group1_digit(mn) >= 0) { group1_insn(mn, &a, &b); return; }
    if (ieq(mn, "test"))      { test_insn(&a, &b); return; }

    if (ieq(mn, "inc") || ieq(mn, "dec")) { incdec_insn(mn, &a); return; }
    if (group3_digit(mn) >= 0)           { one_op_group3(mn, &a); return; }
    if (shift_digit(mn) >= 0)            { shift_insn(mn, &a, &b); return; }

    if (ieq(mn, "push") || ieq(mn, "pop")) { pushpop_insn(mn, &a); return; }
    if (ieq(mn, "call") || ieq(mn, "jmp")) { calljmp_insn(mn, &a); return; }

    {
        int cc = jcc_code(mn);
        if (lc(mn[0]) == 'j' && cc >= 0) {
            emitb(0x0F);
            emitb(0x80 + cc);
            emit_rel32(&a);
            return;
        }
    }

    asm_err2("unknown instruction: ", mn);
}

/* ---------------- data directives ---------------- */

static void emit_value(op_t *o, int nbytes)
{
    u32 v = res_imm(o);
    int i;
    for (i = 0; i < nbytes; i++)
        emitb((v >> (8 * i)) & 0xFF);
}

static void data_insn(int nbytes, char *rest)
{
    char *ops[64];
    int n = split_ops(rest, ops, 64);
    int i;

    for (i = 0; i < n; i++) {
        char *s = ops[i];
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (*s && *s != q) {
                int c;
                if (*s == '\\' && s[1]) {
                    s++;
                    switch (*s) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case '0': c = 0;    break;
                    case '\\': c = '\\'; break;
                    default:  c = *s;   break;
                    }
                } else {
                    c = (u8)*s;
                }
                s++;
                {
                    int k;
                    for (k = 0; k < nbytes; k++)
                        emitb(((u32)c >> (8 * k)) & 0xFF);
                }
            }
        } else {
            op_t o;
            memset(&o, 0, sizeof(o));
            o.base = o.index = -1;
            o.scale = 1;
            parse_op(s, &o);
            emit_value(&o, nbytes);
        }
    }
}

static void res_insn(int unit, char *rest)
{
    int n;
    const char *p;
    int kn;

    p = trim(rest);
    n = (int)parse_num(&p, &kn);
    if (!kn)
        n = 0;

    /* 一个巨大的计数（如 resb 2000000000）会让下面的空转循环跑上亿次，
     * 表现成整个系统卡死。预留空间根本不可能超过 MAX_CODE，直接报错。 */
    if (n > 0 && (u64)(u32)n * (u64)unit > (u64)MAX_CODE) {
        asm_err("reserve count too large");
        return;
    }

    while (n-- > 0) {
        int k;
        for (k = 0; k < unit; k++)
            emitb(0);
    }
}

/* ---------------- line processing ---------------- */

static void strip_comment(char *line)
{
    char q = 0;

    while (*line) {
        if (q) {
            if (*line == q)
                q = 0;
        } else if (*line == '"' || *line == '\'') {
            q = *line;
        } else if (*line == ';') {
            *line = 0;
            return;
        }
        line++;
    }
}

static char *first_word(char **pp)
{
    char *s = trim(*pp);
    char *start = s;

    if (!*s) {
        *pp = s;
        return 0;
    }
    while (*s && !is_sp(*s))
        s++;
    if (*s)
        *s++ = 0;
    *pp = s;
    return start;
}

/* section/segment 指令：section .text | section .data
 * 切段时先把当前段的计数存回，再从中断处恢复另一段的计数。 */
static void set_section(char *rest)
{
    char *p = rest;
    char *nm = first_word(&p);

    if (g_sec == SEC_TEXT)
        g_text_addr = g_addr;
    else
        g_data_addr = g_addr;

    if (!nm) {
        asm_err("section: missing name");
        g_sec = SEC_TEXT;
        g_addr = g_text_addr;
        return;
    }

    if (ieq(nm, ".data") || ieq(nm, "data")) {
        g_sec = SEC_DATA;
        g_addr = g_data_addr;
    } else if (ieq(nm, ".text") || ieq(nm, "text") || ieq(nm, "code")) {
        g_sec = SEC_TEXT;
        g_addr = g_text_addr;
    } else {
        asm_err2("unsupported section: ", nm);
        g_sec = SEC_TEXT;
        g_addr = g_text_addr;
    }
}

static void do_line(char *line)
{
    char *p, *rest, *mn;
    char *q;
    int haslab = 0;

    strip_comment(line);
    p = trim(line);
    if (!*p)
        return;

    {
        int i;
        for (i = 0; p[i] && i < 127; i++)
            g_cur_src[i] = p[i];
        g_cur_src[i] = 0;
    }

    q = p;
    while (*q && !is_sp(*q)) {
        if (*q == ':') { haslab = 1; break; }
        q++;
    }
    if (haslab) {
        char name[40];
        int i = 0;
        *q = 0;
        {
            char *t = trim(p);
            while (t[i] && i < 39) { name[i] = t[i]; i++; }
            name[i] = 0;
        }
        if (name[0])
            sym_add(name, g_addr);
        p = q + 1;
    }

    rest = trim(p);
    if (!*rest)
        return;

    mn = first_word(&rest);
    if (!mn)
        return;

    if (ieq(mn, "section") || ieq(mn, "segment")) {
        set_section(rest);
        return;
    }

    if (ieq(mn, "bits") || ieq(mn, "[bits") || ieq(mn, "global") ||
        ieq(mn, "extern") || ieq(mn, "org") ||
        ieq(mn, "default") || ieq(mn, "cpu")) {
        return;
    }

    if (ieq(mn, "db"))  { data_insn(1, rest); return; }
    if (ieq(mn, "dw"))  { data_insn(2, rest); return; }
    if (ieq(mn, "dd"))  { data_insn(4, rest); return; }
    if (ieq(mn, "resb")) { res_insn(1, rest); return; }
    if (ieq(mn, "resw")) { res_insn(2, rest); return; }
    if (ieq(mn, "resd")) { res_insn(4, rest); return; }

    assemble_insn(mn, rest);
}

static void do_pass(int pass)
{
    const char *s = (const char *)g_src;
    char line[512];

    g_pass = pass;
    g_line = 0;

    /* 每一遍都从 .text 开头开始；第一遍 .data 用占位基址，第二遍用真实基址 */
    g_sec = SEC_TEXT;
    g_text_addr = ORIGIN;
    g_data_addr = (pass == 1) ? ORIGIN : g_data_base;
    g_addr = g_text_addr;

    while (*s) {
        int n = 0;
        while (*s && *s != '\n' && n < 511)
            line[n++] = *s++;
        if (*s == '\n')
            s++;
        line[n] = 0;
        g_line++;
        do_line(line);
    }

    /* 收尾：把当前段的计数存回 */
    if (g_sec == SEC_TEXT)
        g_text_addr = g_addr;
    else
        g_data_addr = g_addr;
}

/* ---------------- entry ---------------- */

__attribute__((section(".text._start"), used, noreturn))
void limasm_entry(void)
{
    char *args, *src, *dst, *p;
    int n;
    u32 code_len, ap;
    pexc_header_t h;

    __asm__ volatile("mov %%eax, %0" : "=r"(ap));
    args = (char *)ap;

    if (!args || !args[0]) {
        sys_ioprintln("limasm: usage: limasm <src.asm> <out.exc>");
        sys_exit(1);
    }

    p   = args;
    src = first_word(&p);
    dst = first_word(&p);
    if (!src || !dst) {
        sys_ioprintln("limasm: usage: limasm <src.asm> <out.exc>");
        sys_exit(1);
    }

    n = sys_fs_read(src, g_src, MAX_SRC - 1);
    if (n < 0) {
        char b[160];
        sprintf(b, "limasm: cannot open %s", src);
        sys_ioprintln(b);
        sys_exit(2);
    }
    if (n == 0) {
        char b[160];
        sprintf(b, "limasm: %s is empty (nothing to assemble)", src);
        sys_ioprintln(b);
        sys_exit(3);
    }
    g_src[n] = 0;

    g_err = 0;
    g_nsym = 0;
    g_err_seen = 0;
    g_err_line = 0;
    g_err_msg[0] = 0;
    g_err_src[0] = 0;
    g_cur_src[0] = 0;

    do_pass(1);

    /* 第一遍结束：.text 大小已定，据此算出 .data 的 VA 基址，
       并把第一遍按占位基址记录下来的数据符号重定位到真实地址。 */
    g_text_size = g_text_addr - ORIGIN;
    g_data_size = g_data_addr - ORIGIN;
    if (g_data_size == 0)
        g_data_base = ORIGIN + g_text_size;             /* 无数据段，不留空洞 */
    else
        g_data_base = (ORIGIN + g_text_size + 3) & ~3u; /* .data 4 字节对齐 */
    {
        int i;
        for (i = 0; i < g_nsym; i++)
            if (g_syms[i].sec == SEC_DATA)
                g_syms[i].addr = g_data_base + (g_syms[i].addr - ORIGIN);
    }

    /* g_out 初值非 0（为了不被 objcopy 丢掉），先把镜像区清零，
       免得 .text 与 .data 之间的对齐填充留下垃圾字节。 */
    memset(g_out + CODE_OFF, 0, MAX_IMAGE + 4);

    do_pass(2);

    code_len = (g_data_base - ORIGIN) + g_data_size;

    if (g_err || g_overflow || code_len == 0 || g_text_size == 0 ||
        code_len > MAX_IMAGE) {
        char buf[200];

        if (g_err && g_err_seen) {
            sprintf(buf, "limasm: error on line %d: %s", g_err_line, g_err_msg);
            sys_ioprintln(buf);
            if (g_err_src[0]) {
                sprintf(buf, "  > %s", g_err_src);
                sys_ioprintln(buf);
            }
        } else if (g_overflow || code_len > MAX_IMAGE) {
            sprintf(buf, "limasm: image too large (.text %u + .data %u, max %u)",
                    g_text_size, g_data_size, MAX_IMAGE);
            sys_ioprintln(buf);
        } else {
            sprintf(buf, "limasm: %s: no code generated "
                         "(no instructions in .text?)", src);
            sys_ioprintln(buf);
        }
        sys_exit(3);
    }

    memset(&h, 0, sizeof(h));
    h.magic[0] = 'P'; h.magic[1] = 'E'; h.magic[2] = 'X'; h.magic[3] = 'C';
    h.version   = PEXC_VERSION;
    h.hdr_size  = PEXC_HDR_SIZE;
    h.arch      = PEXC_ARCH_X86;
    h.bits      = PEXC_BITS_32;
    h.created   = (u32)sys_time();
    h.modified  = h.created;
    h.entry     = ORIGIN;
    h.load      = ORIGIN;
    h.code_size = code_len;
    h.bss_size  = 0;
    h.min_mem_mb  = 0;
    h.min_disk_kb = 0;
    h.priv      = PEXC_PRIV_USER;
    h.flags     = PEXC_FLAG_RING0;
    h.reserved  = 0;
    h.checksum  = pexc_checksum((const u8 *)&h, g_out + CODE_OFF, code_len);

    memcpy(g_out, &h, PEXC_HDR_SIZE);

    n = sys_fs_write(dst, g_out, PEXC_HDR_SIZE + code_len);
    if (n != (int)(PEXC_HDR_SIZE + code_len)) {
        sys_ioprintln("limasm: cannot write output");
        sys_exit(4);
    }

    sys_ioprintln("limasm: ok");
    sys_exit(0);

    for (;;)
        ;
}
