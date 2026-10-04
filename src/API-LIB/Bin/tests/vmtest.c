/* host unit tests for dawnvm: integer ALU, 64-bit ALU, and the
 * integer-only float32 ops (compared bit-exact against the host FPU). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void dwn_host_putc(int c) { (void)c; }

#include "API-LIB/Bin/dawnvm/dwn.h"
#include "API-LIB/Bin/dawnvm/dawnvm.c"

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static int is_nan_bits(uint32_t x) { return (x & 0x7F800000u) == 0x7F800000u && (x & 0x7FFFFFu); }

static long g_ok = 0, g_bad = 0;
static const char *g_cur;

static void fail(const char *what, uint64_t a, uint64_t b, uint64_t got, uint64_t want)
{
    g_bad++;
    if (g_bad <= 25)
        printf("MISMATCH %s/%s a=%llx b=%llx got=%llx want=%llx\n",
               g_cur, what, (unsigned long long)a, (unsigned long long)b,
               (unsigned long long)got, (unsigned long long)want);
}
static void eq(const char *what, uint64_t a, uint64_t b, uint64_t got, uint64_t want)
{
    if (got != want) fail(what, a, b, got, want); else g_ok++;
}

static void chkf(const char *what, uint32_t a, uint32_t b, uint32_t got, uint32_t want)
{
    if (got != want) {
        if (is_nan_bits(want) && is_nan_bits(got)) return;
        fail(what, a, b, got, want);
    } else g_ok++;
}

static void test_float(void)
{
    uint32_t pats[] = {
        0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0xC0000000,
        0x3F000000, 0x7F7FFFFF, 0xFF7FFFFF, 0x00000001, 0x80000001, 0x3F800001,
        0x4B000000, 0x4B7FFFFF, 0x4EFFFFFF, 0x7F800000, 0xFF800000, 0x7FC00000,
        0x33800000, 0x34000000, 0x0A0A0A0A, 0x8A0A0A0A
    };
    int n = (int)(sizeof(pats) / sizeof(pats[0])), i, j, k;
    g_cur = "float";

    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            uint32_t a = pats[i], b = pats[j];
            if (is_nan_bits(a) || is_nan_bits(b)) continue;
            chkf("add", a, b, f32_addsub(a, b, 0), f2u(u2f(a) + u2f(b)));
            chkf("sub", a, b, f32_addsub(a, b, 1), f2u(u2f(a) - u2f(b)));
            chkf("mul", a, b, f32_mul(a, b),     f2u(u2f(a) * u2f(b)));
            if ((b & 0x7FFFFFFFu) != 0)
                chkf("div", a, b, f32_div(a, b), f2u(u2f(a) / u2f(b)));
        }
    for (k = 0; k < 300000; k++) {
        uint32_t a = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        uint32_t b = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        if (((a >> 23) & 0xFF) == 0xFF || ((b >> 23) & 0xFF) == 0xFF) continue;
        chkf("add", a, b, f32_addsub(a, b, 0), f2u(u2f(a) + u2f(b)));
        chkf("sub", a, b, f32_addsub(a, b, 1), f2u(u2f(a) - u2f(b)));
        chkf("mul", a, b, f32_mul(a, b),     f2u(u2f(a) * u2f(b)));
        if ((b & 0x7FFFFFFFu) != 0)
            chkf("div", a, b, f32_div(a, b), f2u(u2f(a) / u2f(b)));
    }
    for (k = 0; k < 200000; k++) {
        uint32_t a = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        uint32_t b = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        float fa = u2f(a), fb = u2f(b);
        int want = (fa != fa || fb != fb) ? 2 : (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
        if (f32_cmp(a, b) != want) fail("cmp", a, b, f32_cmp(a, b), want); else g_ok++;
    }
}

/* ---- 32-bit integer ALU ---- */

static void test_int(void)
{
    int k;
    g_cur = "int";
    for (k = 0; k < 400000; k++) {
        uint32_t x = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        uint32_t y = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        uint32_t r;
        int dst = 5, sh = (int)(y & 31);

        g_vm.flags = 0; g_vm.r[dst] = 0;
        alu(OP_ADD, dst, x, y); r = g_vm.r[dst];
        eq("add", x, y, r, x + y);
        eq("add.c", x, y, (g_vm.flags & F_C) != 0, ((uint64_t)x + y) >> 32 != 0);
        eq("add.o", x, y, (g_vm.flags & F_O) != 0, (~(x ^ y) & (x ^ r) & 0x80000000u) != 0);

        alu(OP_SUB, dst, x, y); r = g_vm.r[dst];
        eq("sub", x, y, r, x - y);
        eq("sub.c", x, y, (g_vm.flags & F_C) != 0, (uint64_t)x < (uint64_t)y);

        alu(OP_AND, dst, x, y); eq("and", x, y, g_vm.r[dst], x & y);
        alu(OP_OR,  dst, x, y); eq("or",  x, y, g_vm.r[dst], x | y);
        alu(OP_XOR, dst, x, y); eq("xor", x, y, g_vm.r[dst], x ^ y);
        alu(OP_MUL, dst, x, y); eq("mul", x, y, g_vm.r[dst], x * y);
        alu(OP_MULU,dst, x, y); eq("mulu",x, y, g_vm.r[dst], x * y);
        alu(OP_SHL, dst, x, sh); eq("shl", x, sh, g_vm.r[dst], shl32(x, sh));
        alu(OP_SHR, dst, x, sh); eq("shr", x, sh, g_vm.r[dst], x >> sh);
        alu(OP_SAR, dst, x, sh); eq("sar", x, sh, g_vm.r[dst], (u32)((i32)x >> sh));
        alu(OP_ROL, dst, x, sh); eq("rol", x, sh, g_vm.r[dst], rol32(x, sh));
        alu(OP_ROR, dst, x, sh); eq("ror", x, sh, g_vm.r[dst], ror32(x, sh));

        if (y) {
            alu(OP_DIVU, dst, x, y); eq("divu", x, y, g_vm.r[dst], x / y);
            alu(OP_REM,  dst, x, y); eq("rem",  x, y, g_vm.r[dst], x % y);
            if (!(x == 0x80000000u && y == 0xFFFFFFFFu)) {
                alu(OP_DIV, dst, x, y); eq("div", x, y, g_vm.r[dst], (u32)((i32)x / (i32)y));
            }
        }

        /* CMP / TEST 不改寄存器，只置标志 */
        g_vm.r[dst] = 0xDEADBEEF; g_vm.flags = 0;
        alu(OP_CMP, dst, x, y);
        eq("cmp.wr", x, y, g_vm.r[dst], 0xDEADBEEF);
        eq("cmp.z", x, y, (g_vm.flags & F_Z) != 0, (x == y));
        eq("cmp.s", x, y, (g_vm.flags & F_S) != 0, ((x - y) & 0x80000000u) != 0);
        eq("cmp.c", x, y, (g_vm.flags & F_C) != 0, (x < y));
        /* 条件码必须还原出正确的有/无符号比较 */
        eq("cc.lt",  x, y, cond_true(CC_LT),  (i32)x <  (i32)y);
        eq("cc.le",  x, y, cond_true(CC_LE),  (i32)x <= (i32)y);
        eq("cc.gt",  x, y, cond_true(CC_GT),  (i32)x >  (i32)y);
        eq("cc.ge",  x, y, cond_true(CC_GE),  (i32)x >= (i32)y);
        eq("cc.ltu", x, y, cond_true(CC_LTU), x <  y);
        eq("cc.leu", x, y, cond_true(CC_LEU), x <= y);
        eq("cc.gtu", x, y, cond_true(CC_GTU), x >  y);
        eq("cc.geu", x, y, cond_true(CC_GEU), x >= y);
    }
}

/* ---- 64-bit ALU ---- */

static void test_int64(void)
{
    int k, dst = 0;
    g_cur = "int64";
    for (k = 0; k < 300000; k++) {
        uint64_t x = ((uint64_t)((uint32_t)rand() << 16) ^ (uint32_t)rand()) |
                     ((uint64_t)(((uint32_t)rand() << 16) ^ (uint32_t)rand()) << 32);
        uint64_t y = ((uint64_t)((uint32_t)rand() << 16) ^ (uint32_t)rand()) |
                     ((uint64_t)(((uint32_t)rand() << 16) ^ (uint32_t)rand()) << 32);
        uint32_t sh = (uint32_t)(y & 63);

        alu64(OP_ADD, dst, x, y); eq("add", x, y, rdreg64(dst), x + y);
        alu64(OP_SUB, dst, x, y); eq("sub", x, y, rdreg64(dst), x - y);
        alu64(OP_AND, dst, x, y); eq("and", x, y, rdreg64(dst), x & y);
        alu64(OP_OR,  dst, x, y); eq("or",  x, y, rdreg64(dst), x | y);
        alu64(OP_XOR, dst, x, y); eq("xor", x, y, rdreg64(dst), x ^ y);
        alu64(OP_NOT, dst, x, 0); eq("not", x, 0, rdreg64(dst), ~x);
        alu64(OP_NEG, dst, x, 0); eq("neg", x, 0, rdreg64(dst), (uint64_t)0 - x);
        alu64(OP_INC, dst, x, 0); eq("inc", x, 0, rdreg64(dst), x + 1);
        alu64(OP_DEC, dst, x, 0); eq("dec", x, 0, rdreg64(dst), x - 1);
        alu64(OP_SHL, dst, x, sh); eq("shl", x, sh, rdreg64(dst), x << sh);
        alu64(OP_SHR, dst, x, sh); eq("shr", x, sh, rdreg64(dst), x >> sh);
        alu64(OP_SAR, dst, x, sh); eq("sar", x, sh, rdreg64(dst), (uint64_t)((i64)x >> sh));

        alu64(OP_CMP, dst, x, y);
        eq("cmp.z", x, y, (g_vm.flags & F_Z) != 0, (x == y));
        eq("cmp.s", x, y, (g_vm.flags & F_S) != 0, ((x - y) & (1ull << 63)) != 0);
        eq("cmp.c", x, y, (g_vm.flags & F_C) != 0, (x < y));
        eq("cc.lt",  x, y, cond_true(CC_LT),  (i64)x <  (i64)y);
        eq("cc.le",  x, y, cond_true(CC_LE),  (i64)x <= (i64)y);
        eq("cc.gt",  x, y, cond_true(CC_GT),  (i64)x >  (i64)y);
        eq("cc.ge",  x, y, cond_true(CC_GE),  (i64)x >= (i64)y);
        eq("cc.ltu", x, y, cond_true(CC_LTU), x <  y);
        eq("cc.leu", x, y, cond_true(CC_LEU), x <= y);
    }
}

int main(void)
{
    test_float();
    printf("float: ok=%ld bad=%ld\n", g_ok, g_bad);
    test_int();
    printf("after int: ok=%ld bad=%ld\n", g_ok, g_bad);
    test_int64();
    printf("TOTAL      ok=%ld bad=%ld\n", g_ok, g_bad);
    return g_bad ? 1 : 0;
}
