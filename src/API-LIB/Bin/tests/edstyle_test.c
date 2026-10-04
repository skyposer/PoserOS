/* host test for EditStyleVM (edstyle.c): verify token coverage and styles.
 * build: clang -IAPI-LIB/Bin/poser_shell -IAPI-LIB/Pstd \
 *            API-LIB/Bin/poser_shell/edstyle.c API-LIB/Bin/tests/edstyle_test.c \
 *            -o edstyle_test && ./edstyle_test
 */
#include <stdio.h>
#include "edstyle.h"

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static const char *sname(int s)
{
    static const char *n[EDST_MAX] = {
        "NORMAL", "KEYWORD", "TYPE", "NUMBER", "STRING", "COMMENT",
        "PREPROC", "FUNC", "OPERATOR", "REGISTER", "LABEL", "SECTION"
    };
    return (s >= 0 && s < EDST_MAX) ? n[s] : "?";
}

static int g_ok, g_bad;

static void show(const char *path, const char *line)
{
    edtok_t t[EDSTYLE_MAX_TOK];
    int len = slen(line);
    int n = edstyle_line(path, line, len, t, EDSTYLE_MAX_TOK);
    int i, cov = 0;

    printf("%-12s |%s|\n", edstyle_lang(path), line);
    if (!edstyle_lang(path)[0]) {
        /* 无可高亮语言：引擎返回 0，渲染层按全 NORMAL 处理 */
        if (n != 0) {
            printf("  !! expected 0 tokens for plain text\n");
            g_bad++;
        } else {
            g_ok++;
        }
        return;
    }
    for (i = 0; i < n; i++) {
        if (t[i].start != cov) {
            printf("  !! gap at %d (token %d starts %d)\n", cov, i, t[i].start);
            g_bad++;
        }
        printf("  %-9s %2d..%2d  \"%.*s\"\n",
               sname(t[i].style), t[i].start, t[i].start + t[i].len,
               t[i].len, line + t[i].start);
        cov = t[i].start + t[i].len;
    }
    if (cov != len) {
        printf("  !! coverage %d != len %d\n", cov, len);
        g_bad++;
    } else {
        g_ok++;
    }
}

int main(void)
{
    struct { const char *p, *l; } c[] = {
        { "/u/a.c",    "int main(void) { return 0; } // hi" },
        { "/u/a.c",    "#include <stdio.h>" },
        { "/u/a.c",    "char *s = \"hi\\n\";" },
        { "/u/a.cpp",  "class Foo : public Bar { };" },
        { "/u/a.java", "public static void main(String[] a) {}" },
        { "/u/a.dasm", "method main {" },
        { "/u/a.dasm", "    ldi EI0, msg   ; print" },
        { "/u/a.dasm", "loop:" },
        { "/u/a.dasm", "section .data" },
        { "/u/a.asm",  "[bits 32]" },
        { "/u/a.asm",  "    mov eax, 2" },
        { "/u/a.cfg",  "name = poser  # who" },
        { "/u/a.cfg",  "[user]" },
        { "/u/a.txt",  "nothing to highlight" }
    };
    int i;

    edstyle_init();
    for (i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++)
        show(c[i].p, c[i].l);

    edstyle_register("c", 0);                 /* 空注册应被忽略 */
    printf("\ncoverage ok=%d bad=%d\n", g_ok, g_bad);
    return g_bad ? 1 : 0;
}
