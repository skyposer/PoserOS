/*
 * runpack - PoserOS 安装包（.pack）运行器
 *
 *   usage: runpack <pkg.pack> [解包目录]
 *
 * 流程：
 *   1. 读入 .pack，校验并解析容器（pack.c）；
 *   2. 解析 PackageMain.ini，打印包信息；
 *   3. 若 [starting] ready 非空：跑 ReadyLoader.dwn（里面用 intervm 18
 *      切到包模式），完成 UI/扩展模式的切换；
 *   4. 跑 main：
 *        type=DBC（或名字以 .dwn 结尾）-> 直接交给 DawnVM 执行；
 *        否则（ExecutableFile）-> 把包解到解包目录，再 sys_exec 拉起。
 *
 * 解包目录默认 /user/home/pkg。
 */

#include "API-LIB/Pstd/pstd_kapi.h"
#include "API-LIB/Pstd/pstd_string.h"
#include "API-LIB/Pstd/pstd_dwn.h"
#include "API-LIB/Bin/pack/pack.h"

#define PACK_BUF_SIZE (192u * 1024u)
#define INI_BUF_SIZE  8192u
#define DEF_DESTDIR   "/user/home/pkg"

static u8   g_pack[PACK_BUF_SIZE] __attribute__((section(".data"))) = {1};
static char g_ini[INI_BUF_SIZE]   __attribute__((section(".data"))) = {1};

static char *first_word(char **pp)
{
    char *s = *pp;
    char *b;

    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s) {
        *pp = s;
        return 0;
    }
    b = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s)
        *s++ = 0;
    *pp = s;
    return b;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ends_with(const char *s, const char *suf)
{
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    int i;

    if (lf > ls)
        return 0;
    s += ls - lf;
    for (i = 0; i < lf; i++)
        if (lc((unsigned char)s[i]) != lc((unsigned char)suf[i]))
            return 0;
    return 1;
}

/* 逐级建目录，如 "/a/b/c" 依次建 /a、/a/b、/a/b/c */
static void ensure_dir(const char *path)
{
    char tmp[PACK_PATH_MAX + 16];
    int i = 0;

    while (path[i] && i < (int)sizeof(tmp) - 1) {
        tmp[i] = path[i];
        i++;
    }
    tmp[i] = 0;

    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            sys_fs_mkdir(tmp);
            tmp[i] = '/';
        }
    }
    sys_fs_mkdir(tmp);
}

/* 拼 "root/name"，返回长度 */
static int join_path(char *out, u32 cap, const char *root, const char *name)
{
    u32 o = 0;
    int i = 0;

    while (root[i] && o + 1 < cap)
        out[o++] = root[i++];
    if (o > 0 && out[o - 1] == '/')
        o--;
    while (*name) {
        if (o + 2 >= cap)
            return -1;
        out[o++] = '/';
        while (*name && *name != '/') {
            if (o + 1 >= cap)
                return -1;
            out[o++] = *name++;
        }
        if (*name == '/')
            name++;
    }
    out[o] = 0;
    return (int)o;
}

static int extract_all(const pack_t *p, const char *root)
{
    u32 i;
    int bad = 0;

    ensure_dir(root);
    for (i = 0; i < p->count; i++) {
        const pack_ent_t *e = &p->ent[i];
        char path[PACK_PATH_MAX + 40];

        if (join_path(path, sizeof(path), root, e->name) < 0) {
            bad++;
            continue;
        }
        if (e->type == PACK_TYPE_DIR) {
            ensure_dir(path);
        } else {
            char parent[PACK_PATH_MAX + 40];
            int k = (int)strlen(path);
            while (k > 0 && path[k - 1] != '/')
                k--;
            if (k > 1) {
                memcpy(parent, path, (usize)k);
                parent[k] = 0;
                ensure_dir(parent);
            }
            if (sys_fs_crt(path) < 0 ||
                sys_fs_write(path, p->buf + e->off, e->size) != (int)e->size)
                bad++;
        }
    }
    return bad;
}

/* 跑一个归档里的 .dwn（直接从内存交给 VM） */
static int run_dwn(const pack_t *p, const pack_ent_t *e)
{
    return dwn_run(p->buf + e->off, e->size);
}

__attribute__((section(".text._start"), used, noreturn))
void runpack_entry(void)
{
    char *args, *p_arg, *dest = DEF_DESTDIR;
    const pack_ent_t *e;
    pack_ini_t ini;
    pack_t pk;
    char b[200];
    int n, rc;

    __asm__ volatile("mov %%eax, %0" : "=r"(args));
    args = (char *)args;

    if (!args || !args[0]) {
        sys_ioprintln("runpack: usage: runpack <pkg.pack> [destdir]");
        sys_exit(1);
    }
    p_arg = args;
    args = first_word(&p_arg);
    {
        char *d = first_word(&p_arg);
        if (d)
            dest = d;
    }

    n = sys_fs_read(args, g_pack, PACK_BUF_SIZE);
    if (n <= 0) {
        sprintf(b, "runpack: cannot read %s", args);
        sys_ioprintln(b);
        sys_exit(2);
    }

    rc = pack_parse(g_pack, (u32)n, &pk);
    if (rc != PACK_OK) {
        sprintf(b, "runpack: %s is not a valid .pack (error %d)", args, rc);
        sys_ioprintln(b);
        sys_exit(3);
    }

    if (pack_find(&pk, PACK_INI_FILE) == 0) {
        sys_ioprintln("runpack: missing PackageMain.ini");
        sys_exit(4);
    }
    e = pack_find(&pk, PACK_INI_FILE);
    if (e->size >= INI_BUF_SIZE) {
        sys_ioprintln("runpack: PackageMain.ini too large");
        sys_exit(4);
    }
    {
        u32 got = 0;
        pack_read(&pk, e, g_ini, INI_BUF_SIZE - 1, &got);
        g_ini[got] = 0;
    }
    pack_ini_parse(g_ini, &ini);

    sprintf(b, "runpack: %s %s [%s/%s]",
            ini.name[0] ? ini.name : "(unnamed)",
            ini.version, ini.sys, ini.arch);
    sys_ioprintln(b);

    /* ready：切包模式的引导字节码，先跑 */
    if (ini.ready[0]) {
        e = pack_find(&pk, ini.ready);
        if (!e) {
            sprintf(b, "runpack: ready file not found: %s", ini.ready);
            sys_ioprintln(b);
            sys_exit(5);
        }
        rc = run_dwn(&pk, e);
        if (rc < 0) {
            sprintf(b, "runpack: ready loader failed (%d)", rc);
            sys_ioprintln(b);
            sys_exit(5);
        }
    } else {
        sys_ioprintln("runpack: no ready loader (running as plain program)");
    }

    if (!ini.main[0]) {
        sys_ioprintln("runpack: no [starting] main, done");
        sys_exit(0);
    }

    e = pack_find(&pk, ini.main);
    if (!e) {
        sprintf(b, "runpack: main file not found: %s", ini.main);
        sys_ioprintln(b);
        sys_exit(6);
    }

    if (ini.type[0] && (ini.type[0] == 'D' || ini.type[0] == 'd') ||
        ends_with(ini.main, ".dwn")) {
        rc = run_dwn(&pk, e);
        if (rc < 0) {
            sprintf(b, "runpack: main exited with error %d", rc);
            sys_exit(7);
        }
        sprintf(b, "runpack: main exited with %d", rc);
        sys_ioprintln(b);
    } else {
        char path[PACK_PATH_MAX + 40];
        int pid;

        if (extract_all(&pk, dest) != 0)
            sys_ioprintln("runpack: warning: some files failed to extract");
        if (join_path(path, sizeof(path), dest, ini.main) < 0) {
            sys_ioprintln("runpack: bad main path");
            sys_exit(6);
        }
        pid = sys_exec(path, "", 0);
        if (pid < 0) {
            sprintf(b, "runpack: exec %s failed (%d)", path, pid);
            sys_ioprintln(b);
            sys_exit(7);
        }
        sys_wait(pid);
    }

    sys_exit(0);

    for (;;)
        ;
}
