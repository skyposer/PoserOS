/*
 * pack.c - .pack 容器解析实现（纯 buf/len，无文件系统 / libc 依赖）
 */

#include "pack.h"

/* ---------------- 小工具 ---------------- */

static int p_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void p_copy(char *d, const char *s, u32 cap)
{
    u32 i = 0;
    while (s[i] && i + 1 < cap) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

static int p_memcpy(void *d, const void *s, u32 n)
{
    u8 *dd = (u8 *)d;
    const u8 *ss = (const u8 *)s;
    u32 i;
    for (i = 0; i < n; i++)
        dd[i] = ss[i];
    return 0;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int p_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lc((unsigned char)*a) != lc((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return lc((unsigned char)*a) == lc((unsigned char)*b);
}

static char *p_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    e = s + p_len(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static u32 r16(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 r32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ---------------- checksum ---------------- */

u32 pack_checksum(const u8 *buf, u32 len)
{
    u32 s = 2166136261u, i;

    for (i = 0; i < len; i++) {
        u8 b = (i >= 28 && i < 32) ? 0u : buf[i];   /* checksum 字段按 0 计 */
        s ^= b;
        s *= 16777619u;
    }
    return s;
}

/* ---------------- 解析 ---------------- */

int pack_parse(const u8 *buf, u32 len, pack_t *p)
{
    u32 i, count, toc, data, total, chk;

    if (!buf || len < PACK_HDR_SIZE)
        return PACK_ERR_SIZE;
    if (buf[0] != PACK_MAGIC0 || buf[1] != PACK_MAGIC1 ||
        buf[2] != PACK_MAGIC2 || buf[3] != PACK_MAGIC3)
        return PACK_ERR_MAGIC;
    if (r16(buf + 4) != PACK_VERSION || r16(buf + 6) != PACK_HDR_SIZE)
        return PACK_ERR_VER;

    count = r32(buf + 8);
    toc   = r32(buf + 12);
    data  = r32(buf + 16);
    total = r32(buf + 24);
    chk   = r32(buf + 28);

    if (count > PACK_MAX_FILES)
        return PACK_ERR_TOC;
    if (toc > len || toc + count * PACK_ENT_SIZE < toc ||
        toc + count * PACK_ENT_SIZE > len)
        return PACK_ERR_TOC;
    if (data > len || (total != 0 && total > len))
        return PACK_ERR_TOC;
    if (pack_checksum(buf, len) != chk)
        return PACK_ERR_CHECK;

    p->buf        = buf;
    p->len        = len;
    p->count      = count;
    p->toc_off    = toc;
    p->data_off   = data;
    p->total_size = total;

    for (i = 0; i < count; i++) {
        const u8 *e = buf + toc + i * PACK_ENT_SIZE;
        pack_ent_t *d = &p->ent[i];
        u32 j;

        for (j = 0; j + 1 < PACK_NAME_MAX && e[j]; j++)
            d->name[j] = (char)e[j];
        d->name[j] = 0;
        d->off  = r32(e + 52);
        d->size = r32(e + 56);
        d->type = r32(e + 60);

        if (d->type != PACK_TYPE_FILE && d->type != PACK_TYPE_DIR)
            return PACK_ERR_ENT;
        if (d->off > len || d->off + d->size < d->off ||
            d->off + d->size > len)
            return PACK_ERR_ENT;
    }
    return PACK_OK;
}

/* ---------------- 查表 / 读取 ---------------- */

int pack_norm_path(const char *in, char *out, u32 cap)
{
    u32 o = 0;
    int i = 0;

    if (!in || !out || cap == 0)
        return -1;

    while (in[i]) {
        int j, seglen;

        while (in[i] == '/' || in[i] == '\\')
            i++;
        if (!in[i])
            break;

        j = i;
        while (in[j] && in[j] != '/' && in[j] != '\\')
            j++;
        seglen = j - i;

        if (seglen == 1 && in[i] == '.') {
            /* "." -> 跳过 */
        } else if (seglen == 2 && in[i] == '.' && in[i + 1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;                        /* 连分隔符一起吃掉 */
        } else {
            if (o > 0) {
                if (o + 1 >= cap)
                    return -1;
                out[o++] = '/';
            }
            if (o + (u32)seglen >= cap)
                return -1;
            while (i < j)
                out[o++] = in[i++];
        }
        i = j;
    }
    out[o] = 0;
    return (int)o;
}

const pack_ent_t *pack_find(const pack_t *p, const char *name)
{
    char norm[PACK_PATH_MAX];
    u32 i;

    if (!p || !name)
        return 0;
    if (pack_norm_path(name, norm, sizeof(norm)) < 0)
        return 0;

    for (i = 0; i < p->count; i++)
        if (p->ent[i].type == PACK_TYPE_FILE && p_ieq(p->ent[i].name, norm))
            return &p->ent[i];
    return 0;
}

int pack_read(const pack_t *p, const pack_ent_t *e, void *dst, u32 cap, u32 *out)
{
    if (!p || !e)
        return PACK_ERR_NOENT;
    if (e->type != PACK_TYPE_FILE)
        return PACK_ERR_TYPE;
    if (e->size > cap)
        return PACK_ERR_FULL;
    p_memcpy(dst, p->buf + e->off, e->size);
    if (out)
        *out = e->size;
    return PACK_OK;
}

/* ---------------- INI ---------------- */

static void ini_set(pack_ini_t *ini, const char *sec, const char *key, char *val)
{
    /* 去掉值两端的引号 */
    if (val[0] == '"' || val[0] == '\'') {
        char q = val[0];
        char *e;
        val++;
        e = val + p_len(val);
        if (e > val && e[-1] == q)
            e[-1] = 0;
    }

    if (p_ieq(sec, "header")) {
        if (p_ieq(key, "sys"))       p_copy(ini->sys, val, sizeof(ini->sys));
        else if (p_ieq(key, "arch")) p_copy(ini->arch, val, sizeof(ini->arch));
    } else if (p_ieq(sec, "info")) {
        if (p_ieq(key, "name"))         p_copy(ini->name, val, sizeof(ini->name));
        else if (p_ieq(key, "version")) p_copy(ini->version, val, sizeof(ini->version));
        else if (p_ieq(key, "author"))  p_copy(ini->author, val, sizeof(ini->author));
    } else if (p_ieq(sec, "starting")) {
        if (p_ieq(key, "ready"))     p_copy(ini->ready, val, sizeof(ini->ready));
        else if (p_ieq(key, "type")) p_copy(ini->type, val, sizeof(ini->type));
        else if (p_ieq(key, "main")) p_copy(ini->main, val, sizeof(ini->main));
    }
}

int pack_ini_parse(const char *text, pack_ini_t *ini)
{
    char sec[24];
    char line[256];
    const char *s = text;
    u32 i;

    if (!text || !ini)
        return PACK_ERR_SIZE;

    for (i = 0; i < sizeof(pack_ini_t); i++)
        ((u8 *)ini)[i] = 0;
    sec[0] = 0;

    while (*s) {
        int n = 0;
        char *p, *eq;

        while (*s && *s != '\n' && n < 255)
            line[n++] = *s++;
        if (*s == '\n')
            s++;
        line[n] = 0;

        p = p_trim(line);
        if (!*p || *p == ';' || *p == '#')
            continue;

        if (*p == '[') {                        /* [section] */
            char *e = p + 1;
            int k = 0;
            while (*e && *e != ']' && k < 23)
                sec[k++] = *e++;
            sec[k] = 0;
            p_trim(sec);
            continue;
        }

        eq = p;
        while (*eq && *eq != '=')
            eq++;
        if (*eq != '=')
            continue;
        *eq = 0;
        p = p_trim(p);
        eq = p_trim(eq + 1);
        ini_set(ini, sec, p, eq);
    }
    return PACK_OK;
}
