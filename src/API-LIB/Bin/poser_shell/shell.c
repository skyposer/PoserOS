#include "API-LIB/Pstd/pstd_kapi.h"
#include "API-LIB/Pstd/pstd_string.h"
#include "edstyle.h"

#define U_ROOT   0
#define U_NORMAL 1

#define HOME_DIR  "/user/home"

#define EDIT_MAX   4096
#define EDIT_ROWS  20
#define EDIT_COLS  80
#define VIEW_COLS  16
#define VIEW_ROWS  18

/* 编辑器屏幕布局：第0行标题，第1行状态，正文 EDIT_ROWS 行，末行底栏 */
#define ED_ROW_STAT 1
#define ED_ROW_BODY 2
#define ED_ROW_FOOT (ED_ROW_BODY + EDIT_ROWS)

#define C_BLUE    1
#define C_PURPLE  5
#define C_WHITE   15
#define C_GREEN   2
#define C_BLACK   0
#define C_GRAY      7
#define C_LIGHTBLUE 9
#define C_LIGHTGREEN 10
#define C_LIGHTCYAN  11
#define C_LIGHTRED   12
#define C_LIGHTMAGENTA 13
#define C_YELLOW     14

/* 配置放在 user/setting 下，不再直接丢家目录 */
#define NAME_FILE "/user/setting/user.cfg"
#define KEY_FILE  "/system/key.cfg"

/* non-zero initialisers keep these in .data so objcopy emits them */
static char g_cwd[256]       = "/user/home";
static char g_name[32]       = "user";
static char g_key[32]        = "666666";
static char g_list[2048]     = {1};
static char g_edit[EDIT_MAX] = {1};
static unsigned int g_edit_len;
static unsigned int g_edit_pos;

void shell_start(char *boot);

__attribute__((section(".text._start"), used, noreturn))
void shell_entry(void)
{
    char *boot;

    __asm__ volatile("mov %%eax, %0" : "=r"(boot));

    shell_start(boot);

    for (;;)
        ;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* 文件名/路径的扩展名是否等于 ext（大小写不敏感，只认最后一段文件名） */
static int ext_is(const char *path, const char *ext)
{
    const char *dot = 0;
    const char *slash = 0;
    const char *p = path;
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
        char c = dot[i];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != ext[i])
            return 0;
        i++;
    }
    return dot[i] == 0;
}

/* 内置识别的文件类型，用于编辑器底部状态栏 */
static const char *file_type(const char *path)
{
    if (ext_is(path, "c"))    return "C Source";
    if (ext_is(path, "cpp"))  return "C++ Source";
    if (ext_is(path, "txt"))  return "Text";
    if (ext_is(path, "cfg"))  return "Config";
    if (ext_is(path, "asm"))  return "Assembly";
    if (ext_is(path, "java")) return "Java Source";
    if (ext_is(path, "exc"))  return "ExecutableFile";
    return "text";
}

/* 按规则切分命令行（原地修改）：
 *   - 空格/tab 划分 token，连续多个算一个
 *   - 未转义的引号（' 或 "）之间的内容算同一个 token，结果里不含引号
 *   - 反斜杠使后一个字符按字面处理（反斜杠本身丢弃）
 * 返回 token 个数，argv[] 以 NULL 结尾。 */
static int tokenize(char *s, char *argv[], int max)
{
    int n = 0;

    while (*s) {
        char *w, *o;

        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        if (n >= max - 1)
            break;

        w = o = s;
        while (*s && *s != ' ' && *s != '\t') {
            char c = *s;

            if (c == '"' || c == '\'') {
                char q = c;
                s++;
                while (*s && *s != q) {
                    if (*s == '\\' && s[1])
                        s++;
                    *o++ = *s++;
                }
                if (*s == q)
                    s++;
                continue;
            }

            if (c == '\\' && s[1])
                s++;
            *o++ = *s++;
        }

        while (*s == ' ' || *s == '\t')
            s++;
        *o = 0;
        argv[n++] = w;
    }

    argv[n] = 0;
    return n;
}

/* 把 argv[from..] 重新拼成一个字符串，单空格分隔 */
static void join_from(char *out, unsigned int max, char **argv, int from)
{
    unsigned int o = 0;
    int i;

    for (i = from; argv[i]; i++) {
        const char *s = argv[i];
        if (i > from && o + 1 < max)
            out[o++] = ' ';
        while (*s && o + 1 < max)
            out[o++] = *s++;
    }
    out[o] = 0;
}

static int dec(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static int do_run(const char *path, const char *args, int ring0)
{
    char b[64];
    int pid;

    pid = sys_exec(path, args, (unsigned int)(ring0 ? 1 : 0));
    if (pid < 0) {
        sprintf(b, "ERR exec failed (%d)", pid);
        sys_ioprintln(b);
        return pid;
    }

    /* the run/exit trace is written to the kernel log file, not the console */
    return sys_wait(pid);
}

static void cmd_su(void)
{
    char pw[48];

    pw[0] = 0;
    sys_ioprint("password: ");
    sys_ioinput(pw, sizeof(pw) - 1);

    if (same(pw, g_key)) {
        sys_set_user(U_ROOT);
        sys_ioprintln("now root");
    } else {
        sys_ioprintln("ERR wrong password");
    }
}

static const char *short_path(const char *path)
{
    const char *p = path;
    const char *s1 = 0;
    const char *s2 = 0;

    while (*p) {
        if (*p == '/') {
            s1 = s2;
            s2 = p;
        }
        p++;
    }

    if (s1 && s1 != path)
        return s1;
    return path;
}

static int prefix(const char *s, const char *pre)
{
    while (*pre) {
        if (*s++ != *pre++)
            return 0;
    }
    return 1;
}

static void copy_str(char *dst, const char *src, unsigned int max)
{
    unsigned int i = 0;

    while (src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void trim_nl(char *s)
{
    unsigned int n = 0;

    while (s[n])
        n++;
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

/* collapse "//", "." and ".." in an absolute path (in place) */
static void path_normalize(char *path)
{
    char out[300];
    unsigned int o = 1, i = 1, seg;

    out[0] = '/';
    while (path[i]) {
        while (path[i] == '/')
            i++;
        if (!path[i])
            break;

        seg = i;
        while (path[i] && path[i] != '/')
            i++;

        if (i - seg == 1 && path[seg] == '.')
            continue;

        if (i - seg == 2 && path[seg] == '.' && path[seg + 1] == '.') {
            if (o > 1) {
                o--;
                while (o > 0 && out[o - 1] != '/')
                    o--;
            }
            continue;
        }

        if (o > 1 && o < sizeof(out) - 1)
            out[o++] = '/';
        while (seg < i && o < sizeof(out) - 1)
            out[o++] = path[seg++];
    }
    out[o] = 0;
    copy_str(path, out, 256);
}

/* "~" / absolute / relative  ->  normalized absolute path */
static char *expand_path(const char *in, char *out, unsigned int outsz)
{
    unsigned int o = 0;

    if (in[0] == '~' && (in[1] == 0 || in[1] == '/')) {
        const char *h = HOME_DIR;
        while (*h && o + 1 < outsz)
            out[o++] = *h++;
        in++;
    } else if (in[0] != '/') {
        const char *c = g_cwd;
        while (*c && o + 1 < outsz)
            out[o++] = *c++;
        if (o == 0 || out[o - 1] != '/') {
            if (o + 1 < outsz)
                out[o++] = '/';
        }
    }

    while (*in && o + 1 < outsz)
        out[o++] = *in++;
    out[o] = 0;

    path_normalize(out);
    return out;
}

/* 路径存在且是文件（目录会被 f_open 拒绝） */
static int is_file(const char *path)
{
    char t[1];
    return sys_fs_read(path, t, 1) >= 0;
}

/* 解析可执行程序路径：
 *   - 以 ~ 或 / 硬性指明开头 -> 按家目录/绝对路径展开；
 *   - 其它情况（没写 ~/ 或 /）-> 优先按相对路径（当前目录）解析，
 *     找不到再回退到系统目录 /system/bin。
 * 找到返回 0 并写入 out，找不到返回 -1。 */
static int resolve_prog(const char *name, char *out, unsigned int outsz)
{
    if (name[0] == '~' || name[0] == '/') {
        expand_path(name, out, outsz);
        return is_file(out) ? 0 : -1;
    }

    expand_path(name, out, outsz);
    if (is_file(out))
        return 0;

    {
        char tmp[256];
        sprintf(tmp, "/system/bin/%s", name);
        if (is_file(tmp)) {
            copy_str(out, tmp, outsz);
            return 0;
        }
    }
    return -1;
}

static void print_cwd(void)
{
    unsigned int hl = 0;

    while (HOME_DIR[hl])
        hl++;

    if (prefix(g_cwd, HOME_DIR) && (g_cwd[hl] == 0 || g_cwd[hl] == '/')) {
        sys_ioprint("~");
        sys_ioprintln(g_cwd + hl);
    } else {
        sys_ioprintln(g_cwd);
    }
}

static void print_prompt(void)
{
    const char *cwd = g_cwd;
    const char *rest = 0;
    const char *sp;
    unsigned int hl = 0;
    int root = (sys_get_user() == U_ROOT);

    while (HOME_DIR[hl])
        hl++;

    if (prefix(cwd, HOME_DIR) && (cwd[hl] == 0 || cwd[hl] == '/'))
        rest = cwd + hl + (cwd[hl] == '/');

    sys_ioprintc(root ? "root" : g_name, C_PURPLE, C_BLACK);
    sys_ioprintc("@", C_PURPLE, C_BLACK);

    if (rest) {
        sys_ioprintc("~", C_WHITE, C_BLACK);
        if (*rest) {
            sys_ioprintc("/", C_WHITE, C_BLACK);
            sys_ioprintc(rest, C_WHITE, C_BLACK);
        }
    } else {
        sp = short_path(cwd);
        if (sp != cwd)
            sys_ioprintc("...", C_WHITE, C_BLACK);
        sys_ioprintc(sp, C_WHITE, C_BLACK);
    }

    sys_ioprintc(root ? "#" : "$", C_GREEN, C_BLACK);
    sys_ioprint(" ");
}

static void print_list(const char *buf)
{
    const char *p = buf;
    int count = 0;
    char cnt[32];

    while (*p) {
        int isdir = (*p == 'D');
        char nm[16];
        char sz[16];
        int k = 0, sk = 0;

        p += 2;                         /* skip "D " / "F " */
        while (*p && *p != ' ' && *p != '\n' && k < 15)
            nm[k++] = *p++;
        nm[k] = 0;

        if (*p == ' ') {
            p++;
            while (*p && *p != '\n' && sk < 15)
                sz[sk++] = *p++;
        }
        sz[sk] = 0;

        /* 首项前空四格、文件之间空两格，避免和命令提示符混淆 */
        sys_ioprint(count == 0 ? "    " : "  ");

        if (isdir) {
            sys_ioprintc(nm, C_BLUE, C_BLACK);
            sys_ioprintc("/", C_BLUE, C_BLACK);
        } else if (ext_is(nm, "exc")) {
            sys_ioprintc(nm, C_GREEN, C_BLACK);
            if (sk) {
                sys_ioprint(" ");
                sys_ioprintc(sz, C_GREEN, C_BLACK);
            }
        } else {
            sys_ioprintc(nm, C_WHITE, C_BLACK);
            if (sk) {
                sys_ioprint(" ");
                sys_ioprintc(sz, C_GREEN, C_BLACK);
            }
        }

        if (*p == '\n')
            p++;
        count++;
    }
    sys_putc('\n');

    sprintf(cnt, "%d items", count);
    sys_ioprintc(cnt, C_GREEN, C_BLACK);
    sys_putc('\n');
}

static void load_account(void)
{
    char tmp[64];
    int n;

    n = sys_fs_read(NAME_FILE, tmp, sizeof(tmp) - 1);
    if (n > 0) {
        tmp[n] = 0;
        trim_nl(tmp);
        if (tmp[0])
            copy_str(g_name, tmp, sizeof(g_name));
    }

    n = sys_fs_read(KEY_FILE, tmp, sizeof(tmp) - 1);
    if (n > 0) {
        tmp[n] = 0;
        trim_nl(tmp);
        if (tmp[0])
            copy_str(g_key, tmp, sizeof(g_key));
    }
}

/* ---------------- text editor ---------------- */

/* 行数 = '\n' 个数 + 1, 所以结尾的 '\n' 后面那行空行也能显示和落光标 */
static int ed_lines(void)
{
    unsigned int i;
    int n = 1;

    for (i = 0; i < g_edit_len; i++)
        if (g_edit[i] == '\n')
            n++;
    return n;
}

/* 光标在第几行 */
static int ed_line_of(unsigned int pos)
{
    unsigned int i;
    int n = 0;

    for (i = 0; i < pos && i < g_edit_len; i++)
        if (g_edit[i] == '\n')
            n++;
    return n;
}

static void ed_ins(char c)
{
    unsigned int i;

    if (g_edit_len + 1 >= EDIT_MAX)
        return;

    for (i = g_edit_len; i > g_edit_pos; i--)
        g_edit[i] = g_edit[i - 1];
    g_edit[g_edit_pos++] = c;
    g_edit_len++;
}

static void ed_back(void)
{
    unsigned int i;

    if (g_edit_pos == 0)
        return;

    for (i = g_edit_pos - 1; i + 1 < g_edit_len; i++)
        g_edit[i] = g_edit[i + 1];
    g_edit_pos--;
    g_edit_len--;
}

/* ---------------- 编辑器绘制（EditStyleVM 高亮 + 局部刷新） ---------------- */

static unsigned int ed_off(int idx);
static int ed_col(unsigned int pos);

/* 高亮样式 -> VGA 前景色 */
static const unsigned char g_ed_fg[EDST_MAX] = {
    C_WHITE,        /* NORMAL   */
    C_LIGHTCYAN,    /* KEYWORD  */
    C_LIGHTGREEN,   /* TYPE     */
    C_LIGHTMAGENTA, /* NUMBER   */
    C_YELLOW,       /* STRING   */
    C_GRAY,         /* COMMENT  */
    C_LIGHTRED,     /* PREPROC  */
    C_LIGHTBLUE,    /* FUNC     */
    C_WHITE,        /* OPERATOR */
    C_LIGHTGREEN,   /* REGISTER */
    C_LIGHTBLUE,    /* LABEL    */
    C_LIGHTRED      /* SECTION  */
};

/* 整行输出：内容补空格到 80 列再一次性写（1 次定位 + 1 次带色输出） */
static void ed_row(int row, const char *s, unsigned int fg)
{
    char b[EDIT_COLS + 1];
    int i, n = 0;

    while (s[n] && n < EDIT_COLS) {
        b[n] = s[n];
        n++;
    }
    for (i = n; i < EDIT_COLS; i++)
        b[i] = ' ';
    b[EDIT_COLS] = 0;
    sys_gotoxy(row, 0);
    sys_ioprintc(b, fg, C_BLACK);
}

/* 光标列夹到屏幕内（不做横向滚动） */
static int ed_curcol(int col)
{
    return (col < EDIT_COLS) ? col : EDIT_COLS - 1;
}

/* 画一行正文：按 EditStyleVM 的 token 上色，光标列反色；行尾补齐 */
static void ed_paint_line(const char *path, int idx, int top, int curcol)
{
    edtok_t t[EDSTYLE_MAX_TOK];
    unsigned char st[EDIT_COLS];
    unsigned int s, e;
    int len, n, i, col, row, runlen, style, inv;
    char run[EDIT_COLS + 1];

    row = ED_ROW_BODY + (idx - top);
    s = ed_off(idx);
    e = s;
    while (e < g_edit_len && g_edit[e] != '\n')
        e++;
    len = (int)(e - s);
    if (len > EDIT_COLS)
        len = EDIT_COLS;

    for (i = 0; i < EDIT_COLS; i++)
        st[i] = EDST_NORMAL;
    n = edstyle_line(path, g_edit + s, len, t, EDSTYLE_MAX_TOK);
    for (i = 0; i < n; i++) {
        int j;
        for (j = t[i].start; j < t[i].start + t[i].len && j < EDIT_COLS; j++)
            if (j >= 0)
                st[j] = (unsigned char)t[i].style;
    }

    sys_gotoxy(row, 0);
    col = 0;
    while (col < EDIT_COLS) {
        style  = st[col];
        inv    = (col == curcol);
        runlen = 0;
        while (col < EDIT_COLS && runlen < EDIT_COLS) {
            char ch;
            if (st[col] != style || (col == curcol) != inv)
                break;
            ch = (col < len) ? g_edit[s + col] : ' ';
            run[runlen++] = (ch == '\t') ? ' ' : ch;
            col++;
        }
        run[runlen] = 0;
        if (inv)
            sys_ioprintc(run, C_BLACK, C_WHITE);
        else
            sys_ioprintc(run, g_ed_fg[style], C_BLACK);
    }
}

static void ed_status(int top, const char *status)
{
    char b[96];
    int total = ed_lines();
    int cur = ed_line_of(g_edit_pos);
    int pages = (total + EDIT_ROWS - 1) / EDIT_ROWS;

    sprintf(b, "line:%d/%d  page:%d/%d  %s",
            cur + 1, total, top / EDIT_ROWS + 1, pages, status);
    ed_row(ED_ROW_STAT, b, C_WHITE);
}

/* 整屏重画（翻页 / 打开文件时用） */
static void ed_full(const char *path, int top, const char *status)
{
    char b[96];
    int i, total = ed_lines();
    int cur = ed_line_of(g_edit_pos);
    int col = ed_curcol(ed_col(g_edit_pos));

    sys_clear();

    sprintf(b, "=== EDIT %s ===", path);
    ed_row(0, b, C_PURPLE);
    ed_status(top, status);

    for (i = top; i < top + EDIT_ROWS; i++) {
        if (i < total)
            ed_paint_line(path, i, top, (i == cur) ? col : -1);
        else
            ed_row(ED_ROW_BODY + (i - top), "", C_WHITE);
    }

    sprintf(b, "-- type = %s   ctrl+n next  ctrl+l prev  ctrl+s save  ctrl+e exit --",
            file_type(path));
    ed_row(ED_ROW_FOOT, b, C_GREEN);
}

static unsigned int ed_off(int idx)
{
    unsigned int i;
    int n = 0;

    for (i = 0; i < g_edit_len; i++) {
        if (n == idx)
            return i;
        if (g_edit[i] == '\n')
            n++;
    }
    return g_edit_len;
}

/* 光标在当前行内的列号 */
static int ed_col(unsigned int pos)
{
    int c = 0;

    while (pos > 0 && g_edit[pos - 1] != '\n') {
        pos--;
        c++;
    }
    return c;
}

/* 方向键上下移动光标：列尽量保持不变 */
static void ed_move_line(int delta)
{
    int cur = ed_line_of(g_edit_pos);
    int col = ed_col(g_edit_pos);
    int tgt = cur + delta;
    unsigned int p;
    int len = 0;

    if (tgt < 0 || tgt >= ed_lines())
        return;

    p = ed_off(tgt);
    while (p + (unsigned int)len < g_edit_len && g_edit[p + len] != '\n')
        len++;
    if (col > len)
        col = len;
    g_edit_pos = p + (unsigned int)col;
}

static void cmd_edit(const char *path)
{
    char status[48];
    int n, top = 0, follow = 1;
    int dirty_from = 0, dirty_to = -1;   /* 需要重画的行号区间（含端点） */
    int need_full = 1, need_status = 1;

    n = sys_fs_read(path, g_edit, EDIT_MAX - 1);
    g_edit_len = (n > 0) ? (unsigned int)n : 0;
    g_edit_pos = 0;

    sprintf(status, "read %d bytes", g_edit_len);

    for (;;) {
        int c, cur = ed_line_of(g_edit_pos), col = ed_col(g_edit_pos);
        int total = ed_lines();

        /* 光标越出当前页才翻页（不会每帧整屏重画） */
        if (follow && (cur < top || cur >= top + EDIT_ROWS)) {
            top = (cur / EDIT_ROWS) * EDIT_ROWS;
            need_full = 1;
        }

        /* ---- 只重画发生变化的那几行 ---- */
        if (need_full) {
            ed_full(path, top, status);
        } else {
            int last = top + EDIT_ROWS - 1;
            int lo = dirty_from, hi = dirty_to, i;
            if (lo < top)  lo = top;
            if (hi > last) hi = last;
            for (i = lo; i <= hi; i++) {
                if (i < total)
                    ed_paint_line(path, i, top, (i == cur) ? ed_curcol(col) : -1);
                else
                    ed_row(ED_ROW_BODY + (i - top), "", C_WHITE);
            }
            if (need_status)
                ed_status(top, status);
        }
        need_full = 0;
        need_status = 0;
        dirty_from = 0;
        dirty_to = -1;
        status[0] = 0;

        c = sys_getkey();

        if (c == 0x05) {              /* ctrl+e 退出 */
            sys_clear();
            sys_ioprintln("editor closed");
            return;
        }
        if (c == 0x13) {              /* ctrl+s 保存 */
            if (sys_fs_write(path, g_edit, g_edit_len) == (int)g_edit_len)
                sprintf(status, "saved %d bytes", g_edit_len);
            else
                sprintf(status, "save failed");
            need_status = 1;
            continue;
        }
        if (c == 0x0E) {              /* ctrl+n 下一页 */
            if (top + EDIT_ROWS < ed_lines())
                top += EDIT_ROWS;
            follow = 0;
            need_full = 1;
            continue;
        }
        if (c == 0x0C) {              /* ctrl+l 上一页 */
            if (top >= EDIT_ROWS)
                top -= EDIT_ROWS;
            follow = 0;
            need_full = 1;
            continue;
        }

        if (c == 0xE0) {              /* 方向键：只重画新旧两行 */
            int k = sys_getkey();
            int nc, oc = cur;

            if (k == 0x48) {          /* 上 */
                ed_move_line(-1);
            } else if (k == 0x50) {   /* 下 */
                ed_move_line(1);
            } else if (k == 0x4B) {   /* 左 */
                if (g_edit_pos > 0)
                    g_edit_pos--;
            } else if (k == 0x4D) {   /* 右 */
                if (g_edit_pos < g_edit_len)
                    g_edit_pos++;
            } else {
                follow = 1;
                continue;
            }
            follow = 1;
            nc = ed_line_of(g_edit_pos);
            dirty_from = (oc < nc) ? oc : nc;
            dirty_to   = (oc > nc) ? oc : nc;
            if (nc != oc && !need_full)
                need_status = 1;      /* 状态栏 line: 变了 */
            continue;
        }

        {                             /* 编辑内容：只重画受影响的行 */
            int oc = cur, ot = total;

            if (c == '\n')
                ed_ins('\n');
            else if (c == '\b')
                ed_back();
            else if (c >= 32 && c < 127)
                ed_ins((char)c);
            else
                continue;             /* 其它控制键：内容未变，不重画 */

            follow = 1;
            cur = ed_line_of(g_edit_pos);
            total = ed_lines();
            dirty_from = (cur < oc) ? cur : oc;
            if (total != ot) {
                dirty_to = 0x7fffffff;    /* 行数变了：本行往下整块重画 */
                need_status = 1;
            } else {
                dirty_to = (cur > oc) ? cur : oc;
            }
            if (cur != oc)
                need_status = 1;
        }
    }
}

/* ---------------- 字节/16进制查看器 ---------------- */

static void view_draw(const char *path, int top, int hex, int len)
{
    int total = (len + VIEW_COLS - 1) / VIEW_COLS;
    int i, j;
    char b[300];
    char line[80];

    sys_clear();

    sprintf(b, "=== %s %s ===", hex ? "HEX" : "BYTE", path);
    sys_ioprintc(b, C_PURPLE, C_BLACK);
    sys_putc('\n');

    sprintf(b, "type = %s  %d bytes  row %d/%d",
            file_type(path), len, top + 1, total ? total : 1);
    sys_ioprintln(b);

    for (i = 0; i < VIEW_ROWS; i++) {
        int idx = top + i;
        int o = 0;

        if (idx < total) {
            for (j = 0; j < VIEW_COLS; j++) {
                int pos = idx * VIEW_COLS + j;
                unsigned char c;

                if (pos >= len)
                    break;
                c = (unsigned char)g_edit[pos];

                if (hex) {
                    const char *hx = "0123456789ABCDEF";
                    line[o++] = hx[c >> 4];
                    line[o++] = hx[c & 15];
                    line[o++] = ' ';      /* 两个16进制一组, 组间空一格 */
                } else {
                    line[o++] = (c >= 32 && c < 127) ? (char)c : '.';
                }
            }
        }
        line[o] = 0;
        sys_ioprintln(line);
    }

    sys_ioprintc("-- ctrl+n next  ctrl+l prev  ctrl+e exit --", C_GREEN, C_BLACK);
    sys_putc('\n');
}

static void cmd_view(const char *path, int hex)
{
    int n, top = 0, total;

    n = sys_fs_read(path, g_edit, EDIT_MAX - 1);
    g_edit_len = (n > 0) ? (unsigned int)n : 0;
    total = (int)((g_edit_len + VIEW_COLS - 1) / VIEW_COLS);

    for (;;) {
        int c;

        view_draw(path, top, hex, (int)g_edit_len);
        c = sys_getkey();

        if (c == 0x05) {              /* ctrl+e 退出 */
            sys_clear();
            sys_ioprintln("viewer closed");
            return;
        }
        if (c == 0x0E) {              /* ctrl+n 下一页 */
            if (top + VIEW_ROWS < total)
                top += VIEW_ROWS;
            continue;
        }
        if (c == 0x0C) {              /* ctrl+l 上一页 */
            if (top >= VIEW_ROWS)
                top -= VIEW_ROWS;
            continue;
        }
    }
}

/* ---------------- 命令处理函数 ---------------- */

/* 用 f_opendir 探测路径类型：目录 >=0，文件 -1 */
static int is_dir(const char *path)
{
    char t[2];
    return sys_fs_list(path, t, sizeof(t)) >= 0;
}

static void *h_ver(char **argv)
{
    (void)argv;
    sys_ioprintln("PoserOS 0.0.3.0 - ring3 + syscall, /system /user ready");
    return 0;
}

static void *h_echo(char **argv)
{
    char b[512];
    join_from(b, sizeof(b), argv, 1);
    sys_ioprintln(b);
    return 0;
}

static void *h_pwd(char **argv)
{
    (void)argv;
    print_cwd();
    return 0;
}

static void *h_cd(char **argv)
{
    char path[256];

    expand_path(argv[1] ? argv[1] : "~", path, sizeof(path));
    if (sys_fs_list(path, g_list, 32) < 0)
        sys_ioprintln("ERR not a directory");
    else
        copy_str(g_cwd, path, sizeof(g_cwd));
    return 0;
}

static void *h_ls(char **argv)
{
    char path[256];

    expand_path(argv[1] ? argv[1] : ".", path, sizeof(path));
    if (sys_fs_list(path, g_list, sizeof(g_list)) < 0)
        sys_ioprintln("ERR cannot list directory");
    else
        print_list(g_list);
    return 0;
}

static void *h_mkdir(char **argv)
{
    char path[256];

    if (!argv[1]) {
        sys_ioprintln("usage: mkdir <dir>");
        return 0;
    }
    sys_ioprintln(sys_fs_mkdir(expand_path(argv[1], path, sizeof(path))) == 0
                  ? "ok" : "ERR cannot create directory");
    return 0;
}

static void *h_rmdir(char **argv)
{
    char path[256];

    if (!argv[1]) {
        sys_ioprintln("usage: rmdir <dir>");
        return 0;
    }
    expand_path(argv[1], path, sizeof(path));
    if (!is_dir(path)) {
        sys_ioprintln("ERR not a directory");
        return 0;
    }
    sys_ioprintln(sys_fs_rmdir(path) == 0 ? "ok" : "ERR cannot remove directory");
    return 0;
}

static void *h_rm(char **argv)
{
    char path[256];

    if (!argv[1]) {
        sys_ioprintln("usage: rm <file>");
        return 0;
    }
    expand_path(argv[1], path, sizeof(path));
    if (is_dir(path)) {
        sys_ioprintln("ERR is a directory (use rmdir)");
        return 0;
    }
    sys_ioprintln(sys_fs_remove(path) == 0 ? "ok" : "ERR cannot remove file");
    return 0;
}

static void *h_touch(char **argv)
{
    char path[256];
    char t[2];

    if (!argv[1]) {
        sys_ioprintln("usage: touch <file>");
        return 0;
    }
    expand_path(argv[1], path, sizeof(path));
    if (is_dir(path)) {
        sys_ioprintln("ERR is a directory");
        return 0;
    }
    if (sys_fs_read(path, t, 1) >= 0)
        sys_ioprintln("ok");
    else
        sys_ioprintln(sys_fs_crt(path) == 0 ? "ok" : "ERR cannot create file");
    return 0;
}

static void *h_cp(char **argv)
{
    char a[256], b[256], msg[64];
    int n;

    if (!argv[1] || !argv[2]) {
        sys_ioprintln("usage: cp <src> <dst>");
        return 0;
    }
    expand_path(argv[1], a, sizeof(a));
    expand_path(argv[2], b, sizeof(b));
    n = sys_fs_copy(a, b);
    if (n >= 0) {
        sprintf(msg, "ok %d bytes", n);
        sys_ioprintln(msg);
    } else {
        sys_ioprintln("ERR copy failed");
    }
    return 0;
}

static void *h_mv(char **argv)
{
    char a[256], b[256];

    if (!argv[1] || !argv[2]) {
        sys_ioprintln("usage: mv <src> <dst>");
        return 0;
    }
    expand_path(argv[1], a, sizeof(a));
    expand_path(argv[2], b, sizeof(b));
    sys_ioprintln(sys_fs_rename(a, b) == 0 ? "ok" : "ERR cannot move/rename");
    return 0;
}

static void *h_cat(char **argv)
{
    char path[256], buf[512];
    int n;

    if (!argv[1]) {
        sys_ioprintln("usage: cat <file>");
        return 0;
    }
    n = sys_fs_read(expand_path(argv[1], path, sizeof(path)), buf, sizeof(buf) - 1);
    if (n < 0) {
        sys_ioprintln("ERR cannot read file");
        return 0;
    }
    buf[n] = 0;
    sys_ioprintln(buf);
    return 0;
}

static void *h_write(char **argv)
{
    char path[256], text[512];
    unsigned int len;

    if (!argv[1]) {
        sys_ioprintln("usage: write <file> <text>");
        return 0;
    }
    join_from(text, sizeof(text), argv, 2);
    len = strlen(text);
    if (sys_fs_write(expand_path(argv[1], path, sizeof(path)), text, len) == (int)len)
        sys_ioprintln("ok");
    else
        sys_ioprintln("ERR permission denied or write failed");
    return 0;
}

static void *h_edit(char **argv)
{
    char path[256];
    int hex = 0;

    if (!argv[1]) {
        sys_ioprintln("usage: edit <file> [-hex]");
        return 0;
    }
    if (argv[2] && same(argv[2], "-hex"))
        hex = 1;

    expand_path(argv[1], path, sizeof(path));

    /* .exc 默认为字节查看模式，-hex 强制 16 进制查看 */
    if (hex || ext_is(path, "exc"))
        cmd_view(path, hex);
    else
        cmd_edit(path);
    return 0;
}

static void *h_run(char **argv)
{
    char path[256], args[400];

    if (!argv[1]) {
        sys_ioprintln("usage: run <file> [args]");
        return 0;
    }
    if (resolve_prog(argv[1], path, sizeof(path)) < 0) {
        sys_ioprint("ERR not found: ");
        sys_ioprintln(argv[1]);
        return 0;
    }
    join_from(args, sizeof(args), argv, 2);
    do_run(path, args, sys_get_user() == U_ROOT);
    return 0;
}

static void *h_sudo(char **argv)
{
    char path[256], args[400];
    int old;

    if (!argv[1] || !same(argv[1], "run") || !argv[2]) {
        sys_ioprintln("usage: sudo run <file> [args]");
        return 0;
    }
    if (resolve_prog(argv[2], path, sizeof(path)) < 0) {
        sys_ioprint("ERR not found: ");
        sys_ioprintln(argv[2]);
        return 0;
    }
    join_from(args, sizeof(args), argv, 3);
    old = sys_get_user();
    sys_set_user(U_ROOT);
    do_run(path, args, 1);
    sys_set_user(old);
    return 0;
}

static void *h_limasm(char **argv)
{
    char a[256], b[256], prog[256], args[300];

    if (!argv[1] || !argv[2]) {
        sys_ioprintln("usage: limasm <src.asm> <out.exc>");
        return 0;
    }
    /* 程序名没写 ~/ 或 /，按相对路径优先，再回退 /system/bin */
    if (resolve_prog("limasm.exc", prog, sizeof(prog)) < 0) {
        sys_ioprintln("ERR limasm not found (/system/bin/limasm.exc missing)");
        return 0;
    }
    expand_path(argv[1], a, sizeof(a));
    expand_path(argv[2], b, sizeof(b));
    sprintf(args, "%s %s", a, b);
    do_run(prog, args, 0);
    return 0;
}

static void *h_dasm(char **argv)
{
    char a[256], b[256], prog[256], args[600];

    if (!argv[1] || !argv[2]) {
        sys_ioprintln("usage: dasm <src.dasm> <out.dwn>");
        return 0;
    }
    if (resolve_prog("dasm.exc", prog, sizeof(prog)) < 0) {
        sys_ioprintln("ERR dasm not found (/system/bin/dasm.exc missing)");
        return 0;
    }
    expand_path(argv[1], a, sizeof(a));
    expand_path(argv[2], b, sizeof(b));
    sprintf(args, "%s %s", a, b);
    do_run(prog, args, 0);
    return 0;
}

static void *h_rundwn(char **argv)
{
    char path[256], prog[256], args[300];

    if (!argv[1]) {
        sys_ioprintln("usage: rundwn <prog.dwn>");
        return 0;
    }
    if (resolve_prog("rundwn.exc", prog, sizeof(prog)) < 0) {
        sys_ioprintln("ERR rundwn not found (/system/bin/rundwn.exc missing)");
        return 0;
    }
    expand_path(argv[1], path, sizeof(path));
    sprintf(args, "%s", path);
    do_run(prog, args, 0);
    return 0;
}

static void *h_su(char **argv)
{
    (void)argv;
    cmd_su();
    return 0;
}

static void *h_setuser(char **argv)
{
    if (!argv[1]) {
        sys_ioprintln("usage: setuser <name>");
        return 0;
    }
    copy_str(g_name, argv[1], sizeof(g_name));
    if (sys_fs_write(NAME_FILE, g_name, strlen(g_name)) < 0)
        sys_ioprintln("ERR cannot save user name");
    else {
        sys_ioprint("user name: ");
        sys_ioprintln(g_name);
    }
    return 0;
}

static void *h_setkey(char **argv)
{
    if (!argv[1]) {
        sys_ioprintln("usage: setkey <new-password>");
        return 0;
    }
    if (sys_get_user() != U_ROOT) {
        sys_ioprintln("ERR permission denied (root only)");
        return 0;
    }
    copy_str(g_key, argv[1], sizeof(g_key));
    if (sys_fs_write(KEY_FILE, g_key, strlen(g_key)) < 0)
        sys_ioprintln("ERR cannot save root password");
    else
        sys_ioprintln("root password updated");
    return 0;
}

static void *h_whoami(char **argv)
{
    (void)argv;
    sys_ioprintln(sys_get_user() == U_ROOT ? "root" : g_name);
    return 0;
}

static void *h_reboot(char **argv)
{
    (void)argv;
    sys_reboot();
    return 0;
}

/* ---------------- 命令注册表 ---------------- */

typedef struct shell_cmd {
    char  *cmd;                  /* 指向外部存储（.rodata），不是结构体内联 */
    void *(*funp)(char **argv);  /* argv 为 NULL 结尾的参数数组 */
} shell_cmd_t;

static void *h_help(char **argv);

#define HELP_COLS 4
#define HELP_ROWS 6
#define HELP_LAST (HELP_COLS * HELP_ROWS)

static const shell_cmd_t g_cmds[] = {
    { "help",    h_help },
    { "ver",     h_ver },
    { "echo",    h_echo },
    { "pwd",     h_pwd },
    { "cd",      h_cd },
    { "ls",      h_ls },
    { "mkdir",   h_mkdir },
    { "rmdir",   h_rmdir },
    { "rm",      h_rm },
    { "del",     h_rm },
    { "touch",   h_touch },
    { "cp",      h_cp },
    { "mv",      h_mv },
    { "rename",  h_mv },
    { "cat",     h_cat },
    { "write",   h_write },
    { "edit",    h_edit },
    { "run",     h_run },
    { "sudo",    h_sudo },
    { "limasm",  h_limasm },
    { "dasm",    h_dasm },
    { "rundwn",  h_rundwn },
    { "su",      h_su },
    { "setuser", h_setuser },
    { "setkey",  h_setkey },
    { "whoami",  h_whoami },
    { "reboot",  h_reboot },
};

#define NCMD ((int)(sizeof(g_cmds) / sizeof(g_cmds[0])))

static void *h_help(char **argv)
{
    int page = 1, pages, row, col;
    char b[64];

    if (argv[1] && argv[1][0]) {
        const char *s = argv[1];
        if (*s == '-')
            s++;
        page = dec(s);
    }
    if (page < 1)
        page = 1;

    pages = (NCMD + HELP_LAST - 1) / HELP_LAST;
    if (page > pages)
        page = pages;

    for (row = 0; row < HELP_ROWS; row++) {
        for (col = 0; col < HELP_COLS; col++) {
            int idx = (page - 1) * HELP_LAST + row * HELP_COLS + col;
            const char *c;
            int l;

            if (idx >= NCMD)
                break;
            c = g_cmds[idx].cmd;
            sys_ioprint(c);
            l = 0;
            while (c[l])
                l++;
            while (l < 14) {
                sys_ioprint(" ");
                l++;
            }
        }
        sys_putc('\n');
    }

    sprintf(b, "-- page %d/%d  (help <page>) --", page, pages);
    sys_ioprintln(b);
    return 0;
}

static void run_line(char *line)
{
    char *argv[32];
    int n, i;

    n = tokenize(line, argv, 32);
    if (n == 0)
        return;

    for (i = 0; i < NCMD; i++) {
        if (same(argv[0], g_cmds[i].cmd)) {
            g_cmds[i].funp(argv);
            return;
        }
    }

    sys_ioprintln("ERR unknown command");
}

void shell_start(char *boot)
{
    char line[256];

    edstyle_init();              /* 注册 EditStyleVM 内置高亮规则 */

    sys_ioprintln("");
    sys_ioprintln("PoserOS shell (loaded from hdd)");

    load_account();

    if (boot && boot[0]) {
        sys_ioprint("boot: ");
        sys_ioprintln(boot);
        run_line(boot);
    }

    sys_ioprintln("type 'help' for commands");

    for (;;) {
        print_prompt();
        sys_ioinput(line, sizeof(line));
        run_line(line);
    }
}
