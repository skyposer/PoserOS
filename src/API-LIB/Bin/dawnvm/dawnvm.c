/*
 * dawnvm.c - DawnVM 解释器内核（v1）
 *
 * 输入一个 .dwn 镜像，在扁平内存里解码执行 DawnByteCode。
 * 输出通过 1 个钩子交给平台层：dwn_host_putc。
 * 程序结束（HLT 或 intervm 17）不直接杀进程，而是把控制权交还 dwn_run，
 * 由其返回退出码 —— 这样内核 / 安装包也能安全地调用它。
 *
 * v1 范围：整数 ALU、位运算、乘除、float32 浮点、64 位（寄存器对 rN:rN+1）、
 *          访存、分支/调用、栈、SVC。
 * 待补：int/float 类型转换（0x60-0x6F）。
 */

#include "dwn.h"

/* ---------------- 宿主钩子（平台层实现） ---------------- */

extern void dwn_host_putc(int c);

/* 可选钩子：不实现就用下面的弱默认（无输入、模式切换无副作用）。
 * PoserOS 宿主层在 dawnbc.c 里给出强符号实现。
 * 若宿主把 dawnvm.c 和 dawnbc.c 编进同一个翻译单元（如宿主测试），
 * 定义 DWN_NO_WEAK_HOST_HOOKS 关掉这里的默认实现，避免重定义。 */
int  dwn_host_getkey(void);
int  dwn_host_readline(char *buf, int size, int echo);
void dwn_host_setmode(int mode);
void dwn_host_printc(const char *s, int fg, int bg);
void dwn_host_gotoxy(int row, int col);
void dwn_host_clear(void);

#ifndef DWN_NO_WEAK_HOST_HOOKS
__attribute__((weak)) int  dwn_host_getkey(void) { return -1; }
__attribute__((weak)) int  dwn_host_readline(char *buf, int size, int echo)
{
    (void)buf; (void)size; (void)echo;
    return 0;
}
__attribute__((weak)) void dwn_host_setmode(int mode) { (void)mode; }
/* 默认把带颜色输出退化成普通输出：无 UI 的宿主也能跑 */
__attribute__((weak)) void dwn_host_printc(const char *s, int fg, int bg)
{
    (void)fg; (void)bg;
    while (*s)
        dwn_host_putc((unsigned char)*s++);
}
__attribute__((weak)) void dwn_host_gotoxy(int row, int col) { (void)row; (void)col; }
__attribute__((weak)) void dwn_host_clear(void) { }
#endif

/* ---------------- VM 状态 ---------------- */

typedef struct {
    u32 r[DWN_NREG];
    u32 pc;
    u32 flags;          /* bit0 Z, bit1 C, bit2 S, bit3 O */
    u32 entry;
    u32 data_base;
    u32 image_size;
    int halted;         /* HLT / intervm 17 后置位，dwn_run 据此收尾 */
    int exit_code;
    int mode;           /* DWN_MODE_*：intervm 18 切换（包/UI 模式） */
    u8  mem[DWN_MEM_SIZE];
} dwn_vm_t;

#define F_Z 1u
#define F_C 2u
#define F_S 4u
#define F_O 8u

static dwn_vm_t g_vm;

/* 运行模式跨 dwn_run 调用保持：安装包先跑 ReadyLoader 切到包模式，
 * 紧接着跑主程序时仍然处于包模式（否则会被 dwn_run 重置掉）。 */
static int g_mode_persist = DWN_MODE_NORMAL;

/* ---------------- 访存小工具（扁平地址，越界回绕到低 16 位） ---------------- */

#define MA(a) ((a) & (DWN_MEM_SIZE - 1))

static u32 rd8(u32 a)  { return g_vm.mem[MA(a)]; }

static u32 rd32(u32 a)
{
    return (u32)g_vm.mem[MA(a)] |
           ((u32)g_vm.mem[MA(a + 1)] << 8) |
           ((u32)g_vm.mem[MA(a + 2)] << 16) |
           ((u32)g_vm.mem[MA(a + 3)] << 24);
}

static void wr8(u32 a, u32 v) { g_vm.mem[MA(a)] = (u8)v; }

static void wr32(u32 a, u32 v)
{
    g_vm.mem[MA(a)]     = (u8)v;
    g_vm.mem[MA(a + 1)] = (u8)(v >> 8);
    g_vm.mem[MA(a + 2)] = (u8)(v >> 16);
    g_vm.mem[MA(a + 3)] = (u8)(v >> 24);
}

static u64 rd64(u32 a)
{
    return (u64)rd32(a) | ((u64)rd32(a + 4) << 32);
}

static void wr64(u32 a, u64 v)
{
    wr32(a, (u32)v);
    wr32(a + 4, (u32)(v >> 32));
}

/* 64 位值用寄存器对 rN:rN+1（N 低位、N+1 高位）；编号按 16 取模回绕 */
static u64 rdreg64(u32 r)
{
    return (u64)g_vm.r[r & 0xF] | ((u64)g_vm.r[(r + 1) & 0xF] << 32);
}

static void wrreg64(u32 r, u64 v)
{
    g_vm.r[r & 0xF]         = (u32)v;
    g_vm.r[(r + 1) & 0xF]   = (u32)(v >> 32);
}

static u32 fetch8(void)  { return rd8(g_vm.pc++); }
static u32 fetch32(void)
{
    u32 v = fetch8();
    v |= fetch8() << 8;
    v |= fetch8() << 16;
    v |= fetch8() << 24;
    return v;
}

/* ---------------- 标志位 ---------------- */

static void flags_logic(u32 r)
{
    g_vm.flags = (r == 0 ? F_Z : 0) | ((r & 0x80000000u) ? F_S : 0);
}

static void flags_add(u32 a, u32 b, u32 carry, u32 r)
{
    u64 s = (u64)a + (u64)b + (u64)carry;
    u32 f = 0;
    if (s >> 32) f |= F_C;
    if (r == 0) f |= F_Z;
    if (r & 0x80000000u) f |= F_S;
    if (~(a ^ b) & (a ^ r) & 0x80000000u) f |= F_O;
    g_vm.flags = f;
}

static void flags_sub(u32 a, u32 b, u32 borrow, u32 r)
{
    u32 f = 0;
    if ((u64)a < (u64)b + (u64)borrow) f |= F_C;
    if (r == 0) f |= F_Z;
    if (r & 0x80000000u) f |= F_S;
    if ((a ^ b) & (a ^ r) & 0x80000000u) f |= F_O;
    g_vm.flags = f;
}

static int cond_true(u32 cc)
{
    u32 f = g_vm.flags;
    switch (cc) {
    case CC_AL:  return 1;
    case CC_EQ:  return (f & F_Z) != 0;
    case CC_NE:  return (f & F_Z) == 0;
    case CC_LT:  return ((f & F_S) != 0) != ((f & F_O) != 0);
    case CC_LE:  return (f & F_Z) || (((f & F_S) != 0) != ((f & F_O) != 0));
    case CC_GT:  return !(f & F_Z) && (((f & F_S) != 0) == ((f & F_O) != 0));
    case CC_GE:  return ((f & F_S) != 0) == ((f & F_O) != 0);
    case CC_LTU: return (f & F_C) != 0;
    case CC_LEU: return (f & F_C) != 0 || (f & F_Z) != 0;
    case CC_GTU: return (f & F_C) == 0 && (f & F_Z) == 0;
    case CC_GEU: return (f & F_C) == 0;
    case CC_C:   return (f & F_C) != 0;
    case CC_NC:  return (f & F_C) == 0;
    case CC_S:   return (f & F_S) != 0;
    case CC_NS:  return (f & F_S) == 0;
    case CC_O:   return (f & F_O) != 0;
    }
    return 0;
}

/* ---------------- 数值运算 ---------------- */

static u32 shl32(u32 v, u32 n) { return n >= 32 ? 0 : v << n; }
static u32 shr32(u32 v, u32 n) { return n >= 32 ? 0 : v >> n; }
static u32 sar32(u32 v, u32 n) { if (n >= 32) n = 31; return (u32)((i32)v >> n); }
static u32 rol32(u32 v, u32 n) { n &= 31; return n ? ((v << n) | (v >> (32 - n))) : v; }
static u32 ror32(u32 v, u32 n) { n &= 31; return n ? ((v >> n) | (v << (32 - n))) : v; }

/* ---- 浮点：寄存器存 float32 位型。目标机没有 x87/SSE，直接用
 * float/double 会链进 __addsf3 / __mulsf3 之类的软浮点运行时，
 * 所以这里用纯整数位运算自己实现 binary32 的加/减/乘/除/比较。 ---- */

#define F32_SIGN 0x80000000u
#define F32_QNAN 0x7FC00000u
#define F32_INF  0x7F800000u

static int f32_is_nan(u32 x)
{
    return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu) != 0;
}

static int f32_is_inf(u32 x) { return (x & 0x7FFFFFFFu) == F32_INF; }

/* 64 位 / 32 位，逐位长除法；避免 i386 上的 64 位除法运行时 */
static u64 udiv64_32(u64 num, u32 den, u32 *rem)
{
    u64 q = 0;
    u32 r = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        r = (r << 1) | (u32)((num >> i) & 1u);
        if (r >= den) {
            r -= den;
            q |= (1ull << i);
        }
    }
    if (rem)
        *rem = r;
    return q;
}

/* 由 (符号, 整数有效数 P, 指数 exp, sticky) 打包成 float32。
 * 约定 value = P * 2^exp，另有 sticky=1 表示还差一个 (0,2^exp) 的尾数。
 * 只在最终精度上做一次 round-to-nearest-even —— 次正规结果也直接由 P 舍入，
 * 避免"先舍到 24 位、再舍到次正规精度"的二次舍入误差。 */
static u32 f32_pack(u32 sign, u64 P, int exp, int sticky)
{
    int b, E, dsh;
    u64 M, rem, half;

    if (P == 0)
        return sign ? F32_SIGN : 0u;

    if (sticky) {                       /* 用 P 的 bit0 记录"下面还有尾数" */
        P = (P << 1) | 1u;
        exp--;
    }

    b = 63;
    while (!(P & (1ull << b)))
        b--;
    E = exp + b;                        /* 按正规数表示时的无偏指数（value = M*2^(E-23)） */

    if (E < -126) {                     /* 次正规 / 下溢：sub = round(value / 2^-149) */
        int t = exp + 149;

        if (t >= 0) {
            M = P << t;
        } else {
            int s = -t;
            if (s > 64) {
                M = 0;                  /* P/2^s < 1/2 -> 下溢为 0 */
            } else if (s == 64) {
                /* 只有 P > 2^63 才进位到 1；恰好一半向偶 -> 0 */
                M = (P > (1ull << 63)) ? 1u : 0u;
            } else {
                M = P >> s;
                rem = P & ((1ull << s) - 1ull);
                half = 1ull << (s - 1);
                if (rem > half || (rem == half && (M & 1u)))
                    M++;
            }
        }
        if (M == 0)
            return sign ? F32_SIGN : 0u;
        if (M >= (1ull << 23))          /* 进位成最小正规数 2^-126 */
            return (sign ? F32_SIGN : 0u) | (1u << 23);
        return (sign ? F32_SIGN : 0u) | (u32)M;
    }

    dsh = b - 23;                       /* 正规：M = round(P / 2^(b-23)) */
    if (dsh > 0) {
        M = P >> dsh;
        rem = P & ((1ull << dsh) - 1ull);
        half = 1ull << (dsh - 1);
        if (rem > half || (rem == half && (M & 1u)))
            M++;
    } else {
        M = P << (-dsh);
    }
    if (M >> 24) { M >>= 1; E++; }
    if (E > 127)
        return (sign ? F32_SIGN : 0u) | F32_INF;
    return (sign ? F32_SIGN : 0u) | ((u32)(E + 127) << 23) | (u32)(M & 0x007FFFFFu);
}

/* 比较：-1 / 0 / 1；无序返回 2 */
static int f32_cmp(u32 a, u32 b)
{
    u32 sa, sb;

    if (f32_is_nan(a) || f32_is_nan(b))
        return 2;
    if ((a & 0x7FFFFFFFu) == 0 && (b & 0x7FFFFFFFu) == 0)
        return 0;                       /* +0 == -0 */
    sa = a >> 31;
    sb = b >> 31;
    if (sa != sb)
        return sa ? -1 : 1;
    {
        u32 aa = a & 0x7FFFFFFFu, bb = b & 0x7FFFFFFFu;
        int c = (aa < bb) ? -1 : (aa > bb) ? 1 : 0;
        return sa ? -c : c;
    }
}

/* 解包 float32：value = m * 2^e，m 归一化到 [2^23, 2^24)。
 * 支持次正规数；x 为零时返回 0。 */
static int f32_unpack(u32 x, u32 *m, int *e)
{
    u32 frac = x & 0x7FFFFF;
    int ex = (int)((x >> 23) & 0xFF);

    if (ex == 0) {
        int b;
        if (frac == 0)
            return 0;
        b = 22;
        while (!(frac & (1u << b)))
            b--;
        *m = frac << (23 - b);
        *e = b - 172;                   /* frac * 2^-149 归一化后的指数 */
        return 1;
    }
    *m = frac | 0x800000;
    *e = ex - 150;                      /* (ex-127) - 23 */
    return 1;
}

static u32 f32_addsub(u32 a, u32 b, int sub)
{
    int ea, eb, e, d, sticky = 0, sa, sb;
    u64 MA, MB;
    u32 ma, mb, fa, fb, sign;

    if (sub)
        b ^= F32_SIGN;

    if (f32_is_nan(a)) return a | 0x00400000u;
    if (f32_is_nan(b)) return b | 0x00400000u;

    if (f32_is_inf(a) || f32_is_inf(b)) {
        if (f32_is_inf(a) && f32_is_inf(b)) {
            if (((a ^ b) & F32_SIGN) != 0)
                return F32_QNAN;        /* inf - inf */
            return a;
        }
        return f32_is_inf(a) ? a : b;
    }

    fa = a & 0x7FFFFF;
    fb = b & 0x7FFFFF;
    ea = (int)((a >> 23) & 0xFF);
    eb = (int)((b >> 23) & 0xFF);

    if ((ea == 0 && fa == 0) && (eb == 0 && fb == 0)) {
        /* 两个零：只有同为 -0 时结果才是 -0（+0 + -0 = +0） */
        return ((a & F32_SIGN) && (b & F32_SIGN)) ? F32_SIGN : 0u;
    }
    if (ea == 0 && fa == 0) return b;
    if (eb == 0 && fb == 0) return a;

    if (ea == 0) { ma = fa; ea = -126; } else { ma = fa | 0x800000; ea -= 127; }
    if (eb == 0) { mb = fb; eb = -126; } else { mb = fb | 0x800000; eb -= 127; }

    sa = (int)(a >> 31);
    sb = (int)(b >> 31);

    if (ea >= eb) {
        e = ea;
        d = ea - eb;
        MA = (u64)ma << 3;
        MB = (u64)mb << 3;
    } else {
        int t;
        e = eb;
        d = eb - ea;
        MA = (u64)mb << 3;
        MB = (u64)ma << 3;
        t = sa; sa = sb; sb = t;
    }

    if (d >= 27) {
        sticky = (MB != 0);
        MB = sticky ? 1u : 0u;
    } else if (d > 0) {
        sticky = (MB & (((u64)1 << d) - 1)) != 0;
        MB >>= d;
        if (sticky)
            MB |= 1u;
    }

    if (sa == sb) {
        MA += MB;
        sign = (u32)sa;
    } else if (MA >= MB) {
        MA -= MB;
        sign = (u32)sa;
        if (MA == 0) return 0u;
    } else {
        MA = MB - MA;
        sign = (u32)sb;
    }

    return f32_pack(sign, MA, e - 26, 0);
}

static u32 f32_mul(u32 a, u32 b)
{
    u32 sign = (a ^ b) & F32_SIGN;
    u32 ma, mb;
    int ea, eb;

    if (f32_is_nan(a)) return a | 0x00400000u;
    if (f32_is_nan(b)) return b | 0x00400000u;
    if (f32_is_inf(a) || f32_is_inf(b)) {
        if ((a & 0x7FFFFFFFu) == 0 || (b & 0x7FFFFFFFu) == 0)
            return F32_QNAN;            /* inf * 0 */
        return sign | F32_INF;
    }
    if (!f32_unpack(a, &ma, &ea) || !f32_unpack(b, &mb, &eb))
        return sign;                    /* 零 */

    return f32_pack(sign, (u64)ma * (u64)mb, ea + eb, 0);
}

static u32 f32_div(u32 a, u32 b)
{
    u32 sign = (a ^ b) & F32_SIGN;
    u32 ma, mb, rem = 0;
    int ea, eb;
    u64 Q;

    if (f32_is_nan(a)) return a | 0x00400000u;
    if (f32_is_nan(b)) return b | 0x00400000u;
    if (f32_is_inf(a)) {
        if (f32_is_inf(b)) return F32_QNAN;
        return sign | F32_INF;
    }
    if (f32_is_inf(b))
        return sign;
    if (!f32_unpack(b, &mb, &eb)) {     /* 除数为零 */
        if (!(a & 0x7FFFFFFFu)) return F32_QNAN;   /* 0/0 */
        return sign | F32_INF;                      /* x/0 */
    }
    if (!f32_unpack(a, &ma, &ea))
        return sign;                    /* 0/x = 0 */

    /* 取 30 位商，余数作为 sticky 参与舍入 */
    Q = udiv64_32((u64)ma << 30, mb, &rem);
    return f32_pack(sign, Q, ea - eb - 30, rem != 0);
}

/* NaN 无序：把 Z/C/S/O 全置 1，使 EQ/LT/GT 都为假，LE/GE 为真 */
static void flags_fcmp(u32 a, u32 b)
{
    int c = f32_cmp(a, b);
    u32 f = 0;

    if (c == 2)      f = F_Z | F_C | F_S | F_O;
    else if (c < 0)  f = F_C | F_S;
    else if (c == 0) f = F_Z;
    g_vm.flags = f;
}

/* ---- 64 位标志 ---- */

static void flags_logic64(u64 r)
{
    g_vm.flags = (r == 0 ? F_Z : 0) | ((r >> 63) ? F_S : 0);
}

static void flags_add64(u64 a, u64 b, u64 c, u64 r)
{
    u64 s = a + b + c;
    u32 f = 0;
    if (s < a || (c && s == a)) f |= F_C;
    if (r == 0) f |= F_Z;
    if (r >> 63) f |= F_S;
    if (~(a ^ b) & (a ^ r) & 0x8000000000000000ull) f |= F_O;
    g_vm.flags = f;
}

static void flags_sub64(u64 a, u64 b, u64 c, u64 r)
{
    u64 d = a - b;
    u32 f = 0;
    if (a < b || (c && d < c)) f |= F_C;
    if (r == 0) f |= F_Z;
    if (r >> 63) f |= F_S;
    if ((a ^ b) & (a ^ r) & 0x8000000000000000ull) f |= F_O;
    g_vm.flags = f;
}

/* x 为主操作数，y 为第二操作数 */
static void alu(u32 op, u32 dst, u32 x, u32 y)
{
    u32 r = 0;
    int write = 1;

    switch (op) {
    case OP_ADD:  r = x + y; flags_add(x, y, 0, r); break;
    case OP_ADC:  { u32 c = g_vm.flags & F_C; r = x + y + c; flags_add(x, y, c, r); } break;
    case OP_SUB:  r = x - y; flags_sub(x, y, 0, r); break;
    case OP_SBB:  { u32 c = g_vm.flags & F_C; r = x - y - c; flags_sub(x, y, c, r); } break;
    case OP_AND:  r = x & y; flags_logic(r); break;
    case OP_OR:   r = x | y; flags_logic(r); break;
    case OP_XOR:  r = x ^ y; flags_logic(r); break;
    case OP_CMP:  flags_sub(x, y, 0, x - y); write = 0; break;
    case OP_TEST: flags_logic(x & y); write = 0; break;
    case OP_SHL:  r = shl32(x, y); flags_logic(r); break;
    case OP_SHR:  r = shr32(x, y); flags_logic(r); break;
    case OP_SAR:  r = sar32(x, y); flags_logic(r); break;
    case OP_ROL:  r = rol32(x, y); flags_logic(r); break;
    case OP_ROR:  r = ror32(x, y); flags_logic(r); break;
    case OP_NOT:  r = ~x; flags_logic(r); break;
    case OP_NEG:  r = 0 - x; flags_sub(0, x, 0, r); break;
    case OP_INC:  r = x + 1; flags_add(x, 1, 0, r); break;
    case OP_DEC:  r = x - 1; flags_sub(x, 1, 0, r); break;
    case OP_MUL:
    case OP_MULU: r = x * y; flags_logic(r); break;
    case OP_DIV:  r = (u32)(y ? (i32)x / (i32)y : 0); flags_logic(r); break;
    case OP_DIVU: r = y ? x / y : 0; flags_logic(r); break;
    case OP_REM:  r = y ? x % y : 0; flags_logic(r); break;

    case OP_FADD: r = f32_addsub(x, y, 0); flags_logic(r); break;
    case OP_FSUB: r = f32_addsub(x, y, 1); flags_logic(r); break;
    case OP_FMUL: r = f32_mul(x, y);       flags_logic(r); break;
    case OP_FDIV: r = f32_div(x, y);       flags_logic(r); break;
    case OP_FNEG: r = x ^ 0x80000000u;     flags_logic(r); break;
    case OP_FABS: r = x & 0x7FFFFFFFu;     flags_logic(r); break;
    case OP_FCMP: {
        int c = f32_cmp(x, y);
        flags_fcmp(x, y);
        r = (c == 2) ? 0x80000000u : (u32)(i32)c;
        break;
    }
    default:      return;
    }

    if (write)
        g_vm.r[dst] = r;
}

/* 64 位 ALU：x 主操作数，y 第二操作数，结果写寄存器对 dst */
static void alu64(u32 op, u32 dst, u64 x, u64 y)
{
    u64 r = 0;
    int write = 1;

    switch (op) {
    case OP_ADD:  r = x + y; flags_add64(x, y, 0, r); break;
    case OP_ADC:  { u64 c = (g_vm.flags & F_C) ? 1u : 0u; r = x + y + c; flags_add64(x, y, c, r); } break;
    case OP_SUB:  r = x - y; flags_sub64(x, y, 0, r); break;
    case OP_SBB:  { u64 c = (g_vm.flags & F_C) ? 1u : 0u; r = x - y - c; flags_sub64(x, y, c, r); } break;
    case OP_AND:  r = x & y; flags_logic64(r); break;
    case OP_OR:   r = x | y; flags_logic64(r); break;
    case OP_XOR:  r = x ^ y; flags_logic64(r); break;
    case OP_CMP:  flags_sub64(x, y, 0, x - y); write = 0; break;
    case OP_TEST: flags_logic64(x & y); write = 0; break;
    case OP_NOT:  r = ~x; flags_logic64(r); break;
    case OP_NEG:  r = (u64)0 - x; flags_sub64(0, x, 0, r); break;
    case OP_INC:  r = x + 1; flags_add64(x, 1, 0, r); break;
    case OP_DEC:  r = x - 1; flags_sub64(x, 1, 0, r); break;
    case OP_SHL:  r = (y >= 64) ? 0 : (x << y); flags_logic64(r); break;
    case OP_SHR:  r = (y >= 64) ? 0 : (x >> y); flags_logic64(r); break;
    case OP_SAR:  { u32 n = (u32)(y >= 64 ? 63 : y); r = (u64)((i64)x >> n); flags_logic64(r); } break;
    default:      return;
    }

    if (write)
        wrreg64(dst, r);
}

/* ---------------- 短指令（0x80-0xBF，2 字节） ---------------- */

static void exec_short(u32 op, u32 dst, u32 src)
{
    switch (op) {
    case OP_S_INC:  g_vm.r[dst]++; flags_logic(g_vm.r[dst]); break;
    case OP_S_DEC:  g_vm.r[dst]--; flags_logic(g_vm.r[dst]); break;
    case OP_S_NOT:  g_vm.r[dst] = ~g_vm.r[dst]; flags_logic(g_vm.r[dst]); break;
    case OP_S_NEG:  g_vm.r[dst] = (u32)(-(i32)g_vm.r[dst]); flags_logic(g_vm.r[dst]); break;
    case OP_S_PUSH: g_vm.r[DWN_SP] -= 4; wr32(g_vm.r[DWN_SP], g_vm.r[dst]); break;
    case OP_S_POP:  g_vm.r[dst] = rd32(g_vm.r[DWN_SP]); g_vm.r[DWN_SP] += 4; break;
    case OP_S_MOV:  g_vm.r[dst] = g_vm.r[src]; break;
    case OP_S_RET:  g_vm.pc = rd32(g_vm.r[DWN_SP]); g_vm.r[DWN_SP] += 4; break;
    }
}

/* ---------------- 系统调用（intervm / SVC） ----------------
 *
 * ABI：R0 = arg0，R1 = arg1；返回值写回 R0（无返回值的调用不动 R0）。
 * 所有地址参数都是 DawnVM 扁平内存地址。纯整数实现，不用 FPU/libc。
 */

#define SVC_A0  (g_vm.r[0])
#define SVC_A1  (g_vm.r[1])
#define SVC_RET (g_vm.r[0])

static void svc_puts(u32 va)
{
    u32 n = 0;
    for (; n < DWN_MEM_SIZE; n++) {
        u32 c = rd8(va + n);
        if (!c)
            break;
        dwn_host_putc((int)c);
    }
}

static void svc_putmem(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        dwn_host_putc((unsigned char)s[i]);
}

static int vm_strlen(u32 va)
{
    u32 n = 0;
    while (n < DWN_MEM_SIZE && rd8(va + n))
        n++;
    return (int)n;
}

static int vm_strcmp(u32 a, u32 b)
{
    for (;;) {
        u32 ca = rd8(a), cb = rd8(b);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (!ca)
            return 0;
        a++;
        b++;
    }
}

static void vm_strcpy(u32 dst, u32 src)
{
    for (;;) {
        u32 c = rd8(src++);
        wr8(dst++, c);
        if (!c)
            break;
    }
}

static void vm_memzero(u32 p, u32 n)
{
    while (n--)
        wr8(p++, 0);
}

/* 无符号十进制；返回长度（不含结尾 NUL） */
static int u32_to_str(char *buf, u32 v)
{
    char t[12];
    int n = 0, i = 0;

    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) buf[i++] = t[--n];
    buf[i] = 0;
    return i;
}

static int i32_to_str(char *buf, i32 v)
{
    if (v < 0) {
        buf[0] = '-';
        return 1 + u32_to_str(buf + 1, 0u - (u32)v);
    }
    return u32_to_str(buf, (u32)v);
}

static int u32_to_hex(char *buf, u32 v)
{
    char t[8];
    int n = 0, i = 0;

    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v) { u32 d = v & 0xF; t[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10)); v >>= 4; }
    while (n) buf[i++] = t[--n];
    buf[i] = 0;
    return i;
}

/* 字符串 -> i32：跳过前导空白，支持 +/- 号，只认十进制 */
static i32 str_to_i32(u32 va)
{
    u32 c;
    int neg = 0;
    u32 v = 0;

    for (;;) {
        c = rd8(va);
        if (c != ' ' && c != '\t')
            break;
        va++;
    }
    if (c == '-' || c == '+') { neg = (c == '-'); va++; }
    for (;;) {
        c = rd8(va);
        if (c < '0' || c > '9')
            break;
        v = v * 10u + (c - '0');
        va++;
    }
    return neg ? (i32)(0u - v) : (i32)v;
}

/* ---- 浮点 <-> 字符串（纯整数） ---- */

/* 定长十进制大数：d[0] 是最高位，共 DEC_N 位 */
#define DEC_N 96

static void dec_zero(u8 *d) { int i; for (i = 0; i < DEC_N; i++) d[i] = 0; }

static void dec_set(u8 *d, u32 v)
{
    int i = DEC_N;

    dec_zero(d);
    do {
        d[--i] = (u8)(v % 10u);
        v /= 10u;
    } while (v && i > 0);
}

static void dec_mul(u8 *d, u32 m)          /* m <= 10 */
{
    u32 carry = 0;
    int i;
    for (i = DEC_N - 1; i >= 0; i--) {
        u32 v = (u32)d[i] * m + carry;
        d[i] = (u8)(v % 10u);
        carry = v / 10u;
    }
}

static void dec_div(u8 *d, u32 m)          /* m <= 10, 截断 */
{
    u32 rem = 0;
    int i;
    for (i = 0; i < DEC_N; i++) {
        u32 v = rem * 10u + d[i];
        d[i] = (u8)(v / m);
        rem = v % m;
    }
}

/* float32 位型 -> 十进制字符串，保留 prec 位小数；返回长度 */
static int f32_to_str(char *buf, u32 bits, int prec)
{
    u32 sign = bits >> 31;
    u32 e8   = (bits >> 23) & 0xFF;
    u32 frac = bits & 0x7FFFFFu;
    u32 m;
    int e, point = 0, k, p = 0, int_end;
    u8 d[DEC_N];

    if (prec < 0) prec = 0;
    if (prec > 9) prec = 9;

    if (e8 == 0xFF) {
        const char *s = frac ? "nan" : "inf";
        if (sign) buf[p++] = '-';
        while (*s) buf[p++] = *s++;
        buf[p] = 0;
        return p;
    }
    if (e8 == 0 && frac == 0) {
        if (sign) buf[p++] = '-';
        buf[p++] = '0';
        if (prec > 0) {
            buf[p++] = '.';
            for (k = 0; k < prec; k++)
                buf[p++] = '0';
        }
        buf[p] = 0;
        return p;
    }
    if (e8 == 0) { m = frac; e = -149; }
    else         { m = frac | 0x800000u; e = (int)e8 - 150; }

    dec_set(d, m);
    if (e >= 0) {
        for (k = 0; k < e; k++)
            dec_mul(d, 2u);
    } else {
        int P = 52;
        for (k = 0; k < P; k++)
            dec_mul(d, 10u);
        point = P;
        for (k = 0; k < -e; k++)
            dec_div(d, 2u);
    }

    int_end = DEC_N - point;               /* 整数位在 [0, int_end) */
    if (point > prec) {                    /* 对第 prec 位后四舍五入 */
        int ridx = int_end + prec;
        if (ridx < DEC_N && d[ridx] >= 5) {
            int i = ridx - 1;
            while (i >= 0) {
                if (d[i] == 9) { d[i] = 0; i--; }
                else { d[i]++; break; }
            }
        }
    }

    if (sign)
        buf[p++] = '-';
    {
        int is = 0;
        while (is < int_end && d[is] == 0)
            is++;
        if (is == int_end)
            buf[p++] = '0';
        else
            while (is < int_end)
                buf[p++] = (char)('0' + d[is++]);
    }
    if (prec > 0) {
        buf[p++] = '.';
        for (k = 0; k < prec; k++) {
            int idx = int_end + k;
            buf[p++] = (char)('0' + (idx < DEC_N ? d[idx] : 0));
        }
    }
    buf[p] = 0;
    return p;
}

/* 把 value = mant * 10^e10 转成 float32 位型（mant 为十进制有效数） */
static u32 f32_from_dec(u64 mant, int e10, int neg)
{
    u64 sig;
    int scale = 0, k;

    if (mant == 0)
        return neg ? 0x80000000u : 0u;

    sig = mant;
    if (e10 > 0) {
        if (e10 > 64) e10 = 64;
        for (k = 0; k < e10; k++) {
            if (sig <= 1844674407370955161ull)
                sig *= 10u;
            else {
                sig = (sig >> 1) * 10u;
                scale++;
            }
        }
    } else if (e10 < 0) {
        if (e10 < -64) e10 = -64;
        for (k = 0; k < -e10; k++) {
            while (sig <= 1844674407370955161ull) {
                sig <<= 1;
                scale--;
            }
            sig = udiv64_32(sig, 10u, 0);
        }
    }
    if (sig == 0)
        return neg ? 0x80000000u : 0u;
    return f32_pack(neg ? 1u : 0u, sig, scale, 0);
}

static u32 str_to_f32(u32 va)
{
    u32 c;
    int neg = 0, seen_dot = 0, e10 = 0, expneg = 0, exp = 0;
    u64 mant = 0;
    int nd = 0;

    for (;;) {
        c = rd8(va);
        if (c != ' ' && c != '\t')
            break;
        va++;
    }
    if (c == '-' || c == '+') { neg = (c == '-'); va++; }

    for (;;) {
        c = rd8(va);
        if (c == '.') { seen_dot = 1; va++; continue; }
        if (c < '0' || c > '9')
            break;
        if (nd < 18) {
            mant = mant * 10u + (u64)(c - '0');
            nd++;
            if (seen_dot)
                e10--;
        } else if (!seen_dot) {
            e10++;
        }
        va++;
    }

    if (c == 'e' || c == 'E') {
        va++;
        c = rd8(va);
        if (c == '-' || c == '+') { expneg = (c == '-'); va++; }
        for (;;) {
            c = rd8(va);
            if (c < '0' || c > '9')
                break;
            if (exp < 1000)
                exp = exp * 10 + (int)(c - '0');
            va++;
        }
        e10 += expneg ? -exp : exp;
    }

    return f32_from_dec(mant, e10, neg);
}

/* 原地插入排序 */
static void svc_sort_i32(u32 base, u32 count, int desc)
{
    u32 i, j;
    for (i = 1; i < count; i++) {
        i32 key = (i32)rd32(base + i * 4u);
        j = i;
        while (j > 0) {
            i32 prev = (i32)rd32(base + (j - 1) * 4u);
            int before = desc ? (key > prev) : (key < prev);
            if (!before)
                break;
            wr32(base + j * 4u, (u32)prev);
            j--;
        }
        wr32(base + j * 4u, (u32)key);
    }
}

static void svc_sort_f32(u32 base, u32 count, int desc)
{
    u32 i, j;
    for (i = 1; i < count; i++) {
        u32 key = rd32(base + i * 4u);
        j = i;
        while (j > 0) {
            u32 prev = rd32(base + (j - 1) * 4u);
            int c = f32_cmp(prev, key);
            int before = desc ? (c < 0) : (c > 0);
            if (!before)
                break;
            wr32(base + j * 4u, prev);
            j--;
        }
        wr32(base + j * 4u, key);
    }
}

static void do_svc(u32 id)
{
    switch (id) {
    case DWN_SVC_PUTC:                              /* 输出一个字符 */
        dwn_host_putc((int)(SVC_A0 & 0xFF));
        break;
    case DWN_SVC_PRINT:                             /* 输出 NUL 结尾字符串 */
        svc_puts(SVC_A0);
        break;
    case DWN_SVC_READLINE:
    case DWN_SVC_READLINES: {                       /* 读一行 -> R0 = 字节数 */
        u32 off = MA(SVC_A0);
        u32 cap = SVC_A1;
        int n;
        if (cap > DWN_MEM_SIZE - off)
            cap = DWN_MEM_SIZE - off;
        n = dwn_host_readline((char *)&g_vm.mem[off], (int)cap, id == DWN_SVC_READLINE);
        SVC_RET = (u32)(n < 0 ? 0 : n);
        break;
    }
    case DWN_SVC_PRINT_I32: {                       /* 打印有符号整数 */
        char b[16];
        svc_putmem(b, i32_to_str(b, (i32)SVC_A0));
        break;
    }
    case DWN_SVC_PRINT_U32: {                       /* 打印无符号整数 */
        char b[16];
        svc_putmem(b, u32_to_str(b, SVC_A0));
        break;
    }
    case DWN_SVC_PRINT_HEX: {                       /* 打印十六进制 */
        char b[16];
        svc_putmem(b, u32_to_hex(b, SVC_A0));
        break;
    }
    case DWN_SVC_PRINT_F32: {                       /* 打印浮点 */
        char b[64];
        svc_putmem(b, f32_to_str(b, SVC_A0, 6));
        break;
    }
    case DWN_SVC_GETKEY:                            /* 读一个键 -> R0 */
        SVC_RET = (u32)dwn_host_getkey();
        break;
    case DWN_SVC_STRLEN:                            /* strlen -> R0 */
        SVC_RET = (u32)vm_strlen(SVC_A0);
        break;
    case DWN_SVC_STRCMP:                            /* strcmp -> R0 (-1/0/1) */
        SVC_RET = (u32)(i32)vm_strcmp(SVC_A0, SVC_A1);
        break;
    case DWN_SVC_STRCPY:                            /* strcpy -> R0 = dst */
        vm_strcpy(SVC_A0, SVC_A1);
        SVC_RET = SVC_A0;
        break;
    case DWN_SVC_ATOI:                              /* 字符串 -> i32 */
        SVC_RET = (u32)str_to_i32(SVC_A0);
        break;
    case DWN_SVC_ITOA: {                            /* (buf, i32) -> len */
        char b[16];
        int n, i;
        u32 buf = SVC_A0;
        n = i32_to_str(b, (i32)SVC_A1);
        for (i = 0; i < n; i++)
            wr8(buf + (u32)i, (u32)b[i]);
        wr8(buf + (u32)n, 0);
        SVC_RET = (u32)n;
        break;
    }
    case DWN_SVC_UTOA: {                            /* (buf, u32) -> len */
        char b[16];
        int n, i;
        u32 buf = SVC_A0;
        n = u32_to_str(b, SVC_A1);
        for (i = 0; i < n; i++)
            wr8(buf + (u32)i, (u32)b[i]);
        wr8(buf + (u32)n, 0);
        SVC_RET = (u32)n;
        break;
    }
    case DWN_SVC_FTOA: {                            /* (buf, f32 bits) -> len */
        char b[64];
        int n, i;
        u32 buf = SVC_A0;
        n = f32_to_str(b, SVC_A1, 6);
        for (i = 0; i < n; i++)
            wr8(buf + (u32)i, (u32)b[i]);
        wr8(buf + (u32)n, 0);
        SVC_RET = (u32)n;
        break;
    }
    case DWN_SVC_ATOF:                              /* 字符串 -> f32 bits */
        SVC_RET = str_to_f32(SVC_A0);
        break;
    case DWN_SVC_EXIT:                              /* 退出，状态码在 R0 */
        g_vm.halted = 1;
        g_vm.exit_code = (int)SVC_A0;
        break;
    case DWN_SVC_SETMODE:                           /* 切换运行模式（跨 dwn_run 保持） */
        g_vm.mode = (int)SVC_A0;
        g_mode_persist = (int)SVC_A0;
        dwn_host_setmode((int)SVC_A0);
        break;
    case DWN_SVC_TOUPPER: {                         /* 单字符 -> 大写 */
        u32 c = SVC_A0 & 0xFF;
        if (c >= 'a' && c <= 'z') c -= 32;
        SVC_RET = c;
        break;
    }
    case DWN_SVC_TOLOWER: {                         /* 单字符 -> 小写 */
        u32 c = SVC_A0 & 0xFF;
        if (c >= 'A' && c <= 'Z') c += 32;
        SVC_RET = c;
        break;
    }
    case DWN_SVC_STRUPR: {                          /* 字符串原地大写 */
        u32 p = SVC_A0;
        for (;;) {
            u32 c = rd8(p);
            if (!c) break;
            if (c >= 'a' && c <= 'z') wr8(p, c - 32);
            p++;
        }
        SVC_RET = SVC_A0;
        break;
    }
    case DWN_SVC_STRLWR: {                          /* 字符串原地小写 */
        u32 p = SVC_A0;
        for (;;) {
            u32 c = rd8(p);
            if (!c) break;
            if (c >= 'A' && c <= 'Z') wr8(p, c + 32);
            p++;
        }
        SVC_RET = SVC_A0;
        break;
    }
    case DWN_SVC_SORT_I32:                          /* 有符号升序 */
        svc_sort_i32(SVC_A0, SVC_A1, 0);
        break;
    case DWN_SVC_SORT_I32D:                         /* 有符号降序 */
        svc_sort_i32(SVC_A0, SVC_A1, 1);
        break;
    case DWN_SVC_SORT_F32:                          /* 浮点升序 */
        svc_sort_f32(SVC_A0, SVC_A1, 0);
        break;
    case DWN_SVC_SORT_F32D:                         /* 浮点降序 */
        svc_sort_f32(SVC_A0, SVC_A1, 1);
        break;
    case DWN_SVC_MEMZERO:                           /* 内存清零 */
        vm_memzero(SVC_A0, SVC_A1);
        break;
    case DWN_SVC_PRINTC: {                          /* (str, attr) 带颜色输出 */
        char s[256];
        int i = 0;
        u32 p = SVC_A0, attr = SVC_A1;
        if (g_vm.mode != DWN_MODE_PACKAGE) {        /* 非包模式降级为普通输出 */
            svc_puts(SVC_A0);
            break;
        }
        while (i < 255) {
            u32 c = rd8(p++);
            if (!c)
                break;
            s[i++] = (char)c;
        }
        s[i] = 0;
        dwn_host_printc(s, (int)(attr & 0xF), (int)((attr >> 4) & 0xF));
        break;
    }
    case DWN_SVC_GOTOXY:                            /* (row, col) 仅包模式 */
        if (g_vm.mode == DWN_MODE_PACKAGE)
            dwn_host_gotoxy((int)SVC_A0, (int)SVC_A1);
        break;
    case DWN_SVC_CLEAR:                             /* () 仅包模式 */
        if (g_vm.mode == DWN_MODE_PACKAGE)
            dwn_host_clear();
        break;
    case DWN_SVC_GETMODE:                           /* () -> mode */
        SVC_RET = (u32)g_vm.mode;
        break;
    default:
        break;
    }
}

/* ---------------- 单步 ---------------- */

static int step(void)
{
    u32 op = fetch8();
    u32 wide = 0;

    if (op == OP_PFX_EB) { wide = 1; op = fetch8(); }
    if (op == OP_ESC)    { fetch8(); return -1; }   /* 扩展操作码暂未实现 */

    if (op == OP_NOP) return 0;
    if (op == OP_HLT) { g_vm.halted = 1; g_vm.exit_code = 0; return 0; }

    if (op >= 0x80 && op <= 0xBF) {
        u32 b = fetch8();
        exec_short(op, b >> 4, b & 0xF);
        return 0;
    }

    {
        u32 mod  = fetch8();
        u32 kind = DWN_MOD_KIND(mod);
        u32 dst  = DWN_MOD_DST(mod);
        u32 size = DWN_MOD_SIZE(mod);
        int is64 = (size == DWN_SIZE_EB);
        u32 a = 0, b = 0, ea = 0;
        u32 imm = 0;
        u64 imm64 = 0;

        if (kind == DWN_KIND_RRR) {
            u32 r2 = fetch8();
            a = r2 >> 4;
            b = r2 & 0xF;
        } else if (kind == DWN_KIND_IMM) {
            u32 n = wide ? 8u : DWN_SIZE_BYTES(size);
            u32 i;
            for (i = 0; i < n; i++)
                imm64 |= (u64)fetch8() << (8 * i);
            imm = (u32)imm64;
        } else if (kind == DWN_KIND_MEM) {
            u32 r2 = fetch8();
            u32 base = r2 >> 4, index = r2 & 0xF;
            u32 disp;
            if (wide) { fetch32(); disp = fetch32(); }
            else      { disp = fetch32(); }
            ea = disp;
            if (base != 0xF)  ea += g_vm.r[base];
            if (index != 0xF) ea += g_vm.r[index];
        } else { /* SPEC */
            if (op == OP_SVC) { do_svc(fetch8()); return 0; }
            {
                u32 cc  = DWN_MOD_CC(mod);
                u32 rel = fetch32();
                u32 tgt = g_vm.pc + rel;
                if (op == OP_CALL) {
                    g_vm.r[DWN_SP] -= 4;
                    wr32(g_vm.r[DWN_SP], g_vm.pc);
                    g_vm.pc = tgt;
                } else if (cond_true(cc)) {
                    g_vm.pc = tgt;
                }
            }
            return 0;
        }

        switch (op) {
        case OP_LDI: if (is64) wrreg64(dst, imm64); else g_vm.r[dst] = imm; return 0;
        case OP_MOV: if (is64) wrreg64(dst, rdreg64(a)); else g_vm.r[dst] = g_vm.r[a]; return 0;
        case OP_LD:  if (is64) wrreg64(dst, rd64(ea));
                     else g_vm.r[dst] = (size == DWN_SIZE_OB) ? rd8(ea)
                                  : (size == DWN_SIZE_TB) ? (rd8(ea) | (rd8(ea + 1) << 8))
                                  : rd32(ea);
                     return 0;
        case OP_ST:  if (is64) wr64(ea, rdreg64(dst));
                     else if (size == DWN_SIZE_OB) wr8(ea, g_vm.r[dst]);
                     else if (size == DWN_SIZE_TB) { wr8(ea, g_vm.r[dst]); wr8(ea + 1, g_vm.r[dst] >> 8); }
                     else wr32(ea, g_vm.r[dst]);
                     return 0;

        case OP_ADD: case OP_ADC: case OP_SUB: case OP_SBB:
        case OP_AND: case OP_OR:  case OP_XOR: case OP_CMP: case OP_TEST:
        case OP_SHL: case OP_SHR: case OP_SAR:
        case OP_NOT: case OP_NEG: case OP_INC: case OP_DEC:
            if (is64) {
                u64 x = (kind == DWN_KIND_RRR) ? rdreg64(a) : rdreg64(dst);
                u64 y = (kind == DWN_KIND_RRR) ? rdreg64(b) : imm64;
                alu64(op, dst, x, y);
            } else if (kind == DWN_KIND_RRR) {
                alu(op, dst, g_vm.r[a], g_vm.r[b]);
            } else {
                alu(op, dst, g_vm.r[dst], imm);
            }
            return 0;

        case OP_ROL: case OP_ROR:
        case OP_MUL: case OP_MULU: case OP_DIV: case OP_DIVU: case OP_REM:
        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV:
        case OP_FNEG: case OP_FABS: case OP_FCMP:
            if (kind == DWN_KIND_RRR)
                alu(op, dst, g_vm.r[a], g_vm.r[b]);
            else
                alu(op, dst, g_vm.r[dst], imm);
            return 0;

        case OP_PUSH: if (is64) { g_vm.r[DWN_SP] -= 8; wr64(g_vm.r[DWN_SP], rdreg64(dst)); }
                      else     { g_vm.r[DWN_SP] -= 4; wr32(g_vm.r[DWN_SP], g_vm.r[dst]); }
                      return 0;
        case OP_POP:  if (is64) { wrreg64(dst, rd64(g_vm.r[DWN_SP])); g_vm.r[DWN_SP] += 8; }
                      else     { g_vm.r[dst] = rd32(g_vm.r[DWN_SP]);  g_vm.r[DWN_SP] += 4; }
                      return 0;
        case OP_JMP:  g_vm.pc = g_vm.r[dst]; return 0;

        default: return -1;
        }
    }
}

/* ---------------- 入口 ---------------- */

/* FNV-1a：头 32 字节（checksum 字段按 0 计）+ 镜像 */
#define DWN_CHK_OFF 28u
static u32 dwn_checksum(const u8 *img, u32 image_size)
{
    u32 s = 2166136261u, i;

    for (i = 0; i < DWN_HDR_SIZE; i++) {
        u8 b = (i >= DWN_CHK_OFF && i < DWN_CHK_OFF + 4) ? 0u : img[i];
        s ^= b;
        s *= 16777619u;
    }
    for (i = 0; i < image_size; i++) {
        s ^= img[DWN_HDR_SIZE + i];
        s *= 16777619u;
    }
    return s;
}

int dwn_run(const u8 *img, u32 len)
{
    const dwn_header_t *h = (const dwn_header_t *)img;
    u32 i, guard;

    if (len < DWN_HDR_SIZE)
        return -1;
    if (h->magic[0] != DWN_MAGIC0 || h->magic[1] != DWN_MAGIC1 ||
        h->magic[2] != DWN_MAGIC2 || h->magic[3] != DWN_MAGIC3)
        return -2;
    if (h->version != DWN_VERSION || h->hdr_size != DWN_HDR_SIZE)
        return -3;
    if (h->image_size == 0 || DWN_HDR_SIZE + h->image_size > len)
        return -4;
    if (h->image_size > DWN_MEM_SIZE)
        return -5;
    /* checksum 为 0 视为未填（兼容早期测试文件），否则校验 */
    if (h->checksum != 0 && dwn_checksum(img, h->image_size) != h->checksum)
        return -7;

    for (i = 0; i < DWN_NREG; i++)
        g_vm.r[i] = 0;
    for (i = 0; i < DWN_MEM_SIZE; i++)
        g_vm.mem[i] = 0;
    for (i = 0; i < h->image_size; i++)
        g_vm.mem[i] = img[DWN_HDR_SIZE + i];

    g_vm.entry      = h->entry;
    g_vm.data_base  = h->data_base;
    g_vm.image_size = h->image_size;
    g_vm.pc         = h->entry;
    g_vm.flags      = 0;
    g_vm.halted     = 0;
    g_vm.exit_code  = 0;
    g_vm.mode       = g_mode_persist;   /* 包模式由 ReadyLoader 设置后保持 */
    g_vm.r[DWN_SP]  = DWN_MEM_SIZE - 4;

    for (guard = 0; guard < 20000000u; guard++) {
        int rc;

        if (g_vm.halted)
            return g_vm.exit_code;
        rc = step();
        if (rc != 0)
            return rc;
    }
    return -6;
}
