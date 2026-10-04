/*
 * pack_test.c - .pack 容器解析宿主单元测试
 *
 *   clang -DPACK_NO_PSTD_TYPES -I. API-LIB/Bin/tests/pack_test.c \
 *         API-LIB/Bin/pack/pack.c -o pack_test && ./pack_test
 */

#include <stdio.h>
#include <string.h>

/* pack.c 与本文件都要用 -DPACK_NO_PSTD_TYPES 编译（见文件头命令） */
#include "API-LIB/Bin/pack/pack.h"

static int g_ok, g_bad;

static void chk(int cond, const char *what)
{
    if (cond)
        g_ok++;
    else {
        g_bad++;
        printf("FAIL: %s\n", what);
    }
}

/* ---- 在内存里拼一个 .pack ---- */

static u8  g_img[4096];
static u32 g_len;

static void w32(u32 off, u32 v)
{
    g_img[off]     = (u8)v;
    g_img[off + 1] = (u8)(v >> 8);
    g_img[off + 2] = (u8)(v >> 16);
    g_img[off + 3] = (u8)(v >> 24);
}

struct srcfile { const char *name; const char *body; int dir; };

static void build_pack(const struct srcfile *fs, int n)
{
    u32 toc_off = PACK_HDR_SIZE;
    u32 data_off = toc_off + (u32)n * PACK_ENT_SIZE;
    u32 pos = data_off, i, j;

    memset(g_img, 0, sizeof(g_img));

    for (i = 0; i < (u32)n; i++) {
        u32 eo = toc_off + i * PACK_ENT_SIZE;
        u32 len = fs[i].dir ? 0 : (u32)strlen(fs[i].body);

        for (j = 0; fs[i].name[j] && j < PACK_NAME_MAX - 1; j++)
            g_img[eo + j] = (u8)fs[i].name[j];
        w32(eo + 52, pos);
        w32(eo + 56, len);
        w32(eo + 60, fs[i].dir ? PACK_TYPE_DIR : PACK_TYPE_FILE);

        if (!fs[i].dir) {
            memcpy(g_img + pos, fs[i].body, len);
            pos += len;
        }
    }

    g_img[0] = 'P'; g_img[1] = 'A'; g_img[2] = 'C'; g_img[3] = 'K';
    g_img[4] = PACK_VERSION & 0xFF;
    g_img[5] = (PACK_VERSION >> 8) & 0xFF;
    g_img[6] = PACK_HDR_SIZE & 0xFF;
    g_img[7] = (PACK_HDR_SIZE >> 8) & 0xFF;
    w32(8, (u32)n);
    w32(12, toc_off);
    w32(16, data_off);
    w32(20, 0);
    w32(24, pos);

    g_len = pos;
    w32(28, pack_checksum(g_img, g_len));
}

static const char INI[] =
    "; package config\n"
    "[header]\n"
    "sys=PoserOS\n"
    "arch=x86\n"
    "[info]\n"
    "name=Hello Pack\n"
    "version=1.0\n"
    "author=dawn\n"
    "[starting]\n"
    "ready=/ReadyLoader.dwn\n"
    "type=DBC\n"
    "main=/pack/main.dwn\n";

static void test_parse(void)
{
    static const struct srcfile fs[] = {
        { "PackageMain.ini", INI,                        0 },
        { "ReadyLoader.dwn", "DWNready",                 0 },
        { "pack",            "",                         1 },
        { "pack/main.dwn",   "DWNmain",                  0 },
        { "res/greeting.txt","hi pack\n",                0 },
    };
    pack_t p;
    const pack_ent_t *e;
    char buf[64];
    u32 out;
    int rc;

    build_pack(fs, 5);
    rc = pack_parse(g_img, g_len, &p);
    chk(rc == PACK_OK, "parse ok");
    chk(p.count == 5, "count");

    e = pack_find(&p, "/PackageMain.ini");
    chk(e != 0, "find ini");
    e = pack_find(&p, "ReadyLoader.dwn");
    chk(e != 0, "find ready (no slash)");
    e = pack_find(&p, "/pack/./main.dwn");
    chk(e != 0, "find main (normalized)");
    e = pack_find(&p, "/res/greeting.txt");
    chk(e != 0, "find greeting");
    chk(pack_find(&p, "nope.txt") == 0, "find missing");

    e = pack_find(&p, "/pack/main.dwn");
    chk(pack_read(&p, e, buf, sizeof(buf), &out) == PACK_OK, "read ok");
    chk(out == 7 && memcmp(buf, "DWNmain", 7) == 0, "read content");

    /* cap 太小 */
    chk(pack_read(&p, e, buf, 3, 0) == PACK_ERR_FULL, "read full");

    /* 目录项不算文件 */
    chk(pack_find(&p, "pack") == 0, "dir not findable");
}

static void test_ini(void)
{
    pack_ini_t ini;

    pack_ini_parse(INI, &ini);
    chk(strcmp(ini.sys, "PoserOS") == 0, "ini sys");
    chk(strcmp(ini.arch, "x86") == 0, "ini arch");
    chk(strcmp(ini.name, "Hello Pack") == 0, "ini name");
    chk(strcmp(ini.version, "1.0") == 0, "ini version");
    chk(strcmp(ini.author, "dawn") == 0, "ini author");
    chk(strcmp(ini.ready, "/ReadyLoader.dwn") == 0, "ini ready");
    chk(strcmp(ini.type, "DBC") == 0, "ini type");
    chk(strcmp(ini.main, "/pack/main.dwn") == 0, "ini main");
}

static void test_norm(void)
{
    char b[64];
    chk(pack_norm_path("/a/b", b, sizeof(b)) == 3 && strcmp(b, "a/b") == 0, "norm a/b");
    chk(pack_norm_path("a//b///c", b, sizeof(b)) == 5 && strcmp(b, "a/b/c") == 0, "norm collapse");
    chk(pack_norm_path("/a/./b", b, sizeof(b)) == 3 && strcmp(b, "a/b") == 0, "norm dot");
    chk(pack_norm_path("/a/b/../c", b, sizeof(b)) == 3 && strcmp(b, "a/c") == 0, "norm dotdot");
    chk(pack_norm_path("/../x", b, sizeof(b)) == 1 && strcmp(b, "x") == 0, "norm dotdot root");
    chk(pack_norm_path("/", b, sizeof(b)) == 0 && strcmp(b, "") == 0, "norm slash");
}

static void test_bad(void)
{
    pack_t p;
    u8 save;

    build_pack((const struct srcfile[]){ { "a", "x", 0 } }, 1);

    save = g_img[0];
    g_img[0] = 'X';
    chk(pack_parse(g_img, g_len, &p) == PACK_ERR_MAGIC, "bad magic");
    g_img[0] = save;

    w32(28, 0x12345678u);
    chk(pack_parse(g_img, g_len, &p) == PACK_ERR_CHECK, "bad checksum");
    w32(28, pack_checksum(g_img, g_len));

    chk(pack_parse(g_img, 4, &p) == PACK_ERR_SIZE, "short");
}

/* 读一个真实 .pack 并打印，便于和 mkpack.py 交叉验证：
 *   ./pack_test foo.pack */
static int dump_file(const char *path)
{
    static u8 fb[1 << 20];
    char text[4096];
    FILE *f = fopen(path, "rb");
    pack_t p;
    pack_ini_t ini;
    const pack_ent_t *e;
    u32 n, i, out;
    int rc;

    if (!f) {
        printf("pack_test: cannot open %s\n", path);
        return 2;
    }
    n = (u32)fread(fb, 1, sizeof(fb), f);
    fclose(f);

    rc = pack_parse(fb, n, &p);
    printf("pack_test: %s parse rc=%d count=%u\n", path, rc, p.count);
    if (rc != PACK_OK)
        return 1;

    for (i = 0; i < p.count; i++)
        printf("  %c %-28s %u\n", p.ent[i].type ? 'D' : 'F',
               p.ent[i].name, p.ent[i].size);

    e = pack_find(&p, PACK_INI_FILE);
    if (e && e->size < sizeof(text)) {
        pack_read(&p, e, text, sizeof(text) - 1, &out);
        text[out] = 0;
        pack_ini_parse(text, &ini);
        printf("  ini: sys=%s arch=%s name=%s type=%s\n",
               ini.sys, ini.arch, ini.name, ini.type);
        printf("  ini: ready=%s main=%s\n", ini.ready, ini.main);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2)
        return dump_file(argv[1]);
    test_parse();
    test_ini();
    test_norm();
    test_bad();
    printf("pack_test: ok=%d bad=%d\n", g_ok, g_bad);
    return g_bad ? 1 : 0;
}
