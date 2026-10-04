#ifndef PSTD_IO_H
#define PSTD_IO_H

#include "pstd_int.h"
#include "pstd_port.h"

#define COLOR_BLACK      0
#define COLOR_BLUE       1
#define COLOR_GREEN      2
#define COLOR_CYAN       3
#define COLOR_RED        4
#define COLOR_MAGENTA    5
#define COLOR_BROWN      6
#define COLOR_LGRAY      7
#define COLOR_DGRAY      8
#define COLOR_LBLUE      9
#define COLOR_LGREEN     10
#define COLOR_LCYAN      11
#define COLOR_LRED       12
#define COLOR_LMAGENTA   13
#define COLOR_YELLOW     14
#define COLOR_WHITE      15

#define VGA_ADDR   0xB8000
#define VGA_COLS   80
#define VGA_ROWS   25
#define VGA_SIZE   (VGA_COLS * VGA_ROWS)

#define VGA_COLOR(fg, bg)  ((u8)(((bg) << 4) | ((fg) & 0x0F)))
#define VGA_ENTRY(c, attr) ((u16)((u8)(c) | ((u16)(attr) << 8)))

static u16 *const vga     = (u16 *)VGA_ADDR;
static u16 *const vga_end = (u16 *)VGA_ADDR + VGA_SIZE;
static u16 vga_pos = 0;
static u8  vga_attr = 0x0F;

static u8 userFG = COLOR_WHITE;
static u8 userBG = COLOR_BLACK;

static void setColor(u8 fg, u8 bg)
{
    vga_attr = VGA_COLOR(fg, bg);
}

static void clearScreen(void)
{
    u16 *p;

    for (p = vga; p < vga_end; p++)
        *p = VGA_ENTRY(' ', vga_attr);

    vga_pos = 0;
}

/* 把输出光标定位到 (row, col)，供编辑器局部刷新用 */
static void gotoXY(int row, int col)
{
    if (row < 0) row = 0;
    if (row >= VGA_ROWS) row = VGA_ROWS - 1;
    if (col < 0) col = 0;
    if (col >= VGA_COLS) col = VGA_COLS - 1;
    vga_pos = (u16)(row * VGA_COLS + col);
}

/* 整屏上滚一行：第 1..24 行搬到 0..23，末行清空 */
static void scrollUp(void)
{
    u16 *p;

    for (p = vga; p < vga_end - VGA_COLS; p++)
        *p = *(p + VGA_COLS);
    for (; p < vga_end; p++)
        *p = VGA_ENTRY(' ', vga_attr);
}

static void putc(char c)
{
    if (c == '\n') {
        vga_pos += VGA_COLS - (vga_pos % VGA_COLS);
        if (vga_pos >= VGA_SIZE) {
            scrollUp();
            vga_pos = VGA_SIZE - VGA_COLS;
        }
        return;
    }

    if (c == '\b') {
        if (vga_pos) {
            vga_pos--;
            vga[vga_pos] = VGA_ENTRY(' ', vga_attr);
        }
        return;
    }

    if (vga_pos >= VGA_SIZE) {
        scrollUp();
        vga_pos = VGA_SIZE - VGA_COLS;
    }

    vga[vga_pos] = VGA_ENTRY(c, vga_attr);
    vga_pos++;
}

static void ioprint(const char *s)
{
    while (*s)
        putc(*s++);
}

static void ioprintln(const char *s)
{
    while (*s)
        putc(*s++);
    putc('\n');
}

static void ioprintc(const char *s, u8 fg, u8 bg)
{
    u8 old = vga_attr;
    setColor(fg, bg);
    while (*s)
        putc(*s++);
    vga_attr = old;
}

static void ioprintlnc(const char *s, u8 fg, u8 bg)
{
    u8 old = vga_attr;
    setColor(fg, bg);
    while (*s)
        putc(*s++);
    putc('\n');
    vga_attr = old;
}

#define KB_BUF_SIZE 64

#define KB_DATA    0x60
#define KB_STATUS  0x64
#define KB_OBF     0x01
#define KB_SC_EXT  0xE0
#define KB_SC_LSH  0x2A
#define KB_SC_RSH  0x36
#define KB_SC_CTRL 0x1D

/* 扩展键（0xE0 前缀）里我们关心的方向键扫描码 */
#define KB_SC_UP    0x48
#define KB_SC_DOWN  0x50
#define KB_SC_LEFT  0x4B
#define KB_SC_RIGHT 0x4D

/* 方向键在键缓冲里以 0xE0,scancode 两个字节表示 */
#define KB_IS_EXT(c) ((unsigned char)(c) == KB_SC_EXT)

static const char kb_map[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char kb_map_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x39] = ' ',
};

static volatile u8  kb_buf[KB_BUF_SIZE];
static volatile u32 kb_head;
static volatile u32 kb_tail;
static volatile u8  kb_shift;
static volatile u8  kb_ctrl;
static volatile u8  kb_ext;

static void kb_push(u8 c)
{
    u32 n = (kb_head + 1) % KB_BUF_SIZE;

    if (n == kb_tail)
        return;

    kb_buf[kb_head] = c;
    kb_head = n;
}

static int kb_pop(u8 *c)
{
    if (kb_tail == kb_head)
        return 0;

    *c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return 1;
}

static void kb_feed(u8 sc)
{
    char c;

    if (sc == KB_SC_EXT) {
        kb_ext = 1;
        return;
    }

    if (sc == KB_SC_LSH || sc == KB_SC_RSH) {
        kb_shift = 1;
        kb_ext = 0;
        return;
    }
    if (sc == KB_SC_LSH + 0x80 || sc == KB_SC_RSH + 0x80) {
        kb_shift = 0;
        kb_ext = 0;
        return;
    }

    if (sc == KB_SC_CTRL) {
        kb_ctrl = 1;
        kb_ext = 0;
        return;
    }
    if (sc == KB_SC_CTRL + 0x80) {
        kb_ctrl = 0;
        kb_ext = 0;
        return;
    }

    if (sc & 0x80) {
        kb_ext = 0;
        return;
    }

    if (kb_ext) {
        kb_ext = 0;
        /* 方向键保留成 0xE0,scancode 两个字节，交给上层（编辑器）处理；
         * 其它扩展键（右 ctrl/alt、小键盘等）直接丢弃。 */
        if (sc == KB_SC_UP || sc == KB_SC_DOWN ||
            sc == KB_SC_LEFT || sc == KB_SC_RIGHT) {
            kb_push(KB_SC_EXT);
            kb_push(sc);
        }
        return;
    }

    c = kb_shift ? kb_map_shift[sc] : kb_map[sc];

    /* Ctrl + letter becomes the ASCII control code 0x01..0x1A */
    if (c && kb_ctrl) {
        char l = c;
        if (l >= 'a' && l <= 'z')
            l = (char)(l - 32);
        if (l >= 'A' && l <= 'Z')
            c = (char)(l - 'A' + 1);
    }

    if (c)
        kb_push((u8)c);
}

static void kb_poll(void)
{
    while (readp8(KB_STATUS) & KB_OBF)
        kb_feed(readp8(KB_DATA));
}

static int kb_irq_on(void)
{
    u32 f;

    __asm__ volatile("pushfd\npop %0" : "=r"(f));
    return (f & 0x200) != 0;
}

static int kb_getch(void)
{
    u8 c;

    for (;;) {
        if (kb_pop(&c))
            return (int)c;

        if (kb_irq_on())
            __asm__ volatile("hlt");
        else
            kb_poll();
    }
}

static int ioinput(char *buf, u32 size)
{
    u32 n = 0;

    if (!size)
        return 0;

    for (;;) {
        int c = kb_getch();

        if (KB_IS_EXT(c)) {          /* 方向键：忽略，别插进输入行 */
            kb_getch();
            continue;
        }

        if (c == '\n') {
            putc('\n');
            break;
        }

        if (c == '\b') {
            if (n) {
                n--;
                putc('\b');
            }
            continue;
        }

        if (n + 1 < size) {
            buf[n++] = (char)c;
            putc((char)c);
        }
    }

    buf[n] = 0;
    return (int)n;
}

static int ioinputs(char *buf, u32 size)
{
    u32 n = 0;

    while (n < size) {
        int c = kb_getch();

        if (KB_IS_EXT(c)) {          /* 方向键：忽略 */
            kb_getch();
            continue;
        }

        if (c == '\b') {
            if (n) {
                n--;
                putc('\b');
            }
            continue;
        }

        buf[n++] = (char)c;
        putc((char)c);
    }

    return (int)n;
}

#endif
