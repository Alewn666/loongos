#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS 桌面路线 P2 · 图形终端（gfxterm.c）
 *
 * 一个「终端窗口」的完整实现：历史缓冲 + UTF-8 行渲染 + 输入行编辑器 +
 * 命令执行。命令执行走 kernel.c 导出的 shell_dispatch_line()，因此
 * 「图形页面上操作电脑的所有功能」就是字面意思：所有 shell 命令都可用。
 *
 * 预算：历史 12KB + 输入行 200B + 渲染临时行 512B（栈），全部静态/栈，不引分配器。
 * 字库：复用 kernel.c 的 fb_query_glyph()（gfx.c 的 gfx_text），不复制 885 个 CJK 字形。
 */
#include <stdint.h>
#include "gfx.h"
#include "gfx_draw.h"
#include "gfxterm.h"

typedef unsigned int u32;

/* kernel.c 导出：与串口 shell 完全同一个分发入口 */

#define GT_HIST   12288
#define GT_LINE   200
#define GT_ROWBUF 512
#define GT_MAXROW 64
#define LH        18                  /* 16px 字形 + 2px 行距 */
#define INPUT_H   22

#define C_BG      GFX_RGB(14, 23, 38)
#define C_TEXT    GFX_RGB(207, 224, 242)
#define C_DIM     GFX_RGB(122, 138, 166)
#define C_PROMPT  GFX_RGB(86, 214, 176)
#define C_INPUTBG GFX_RGB(20, 33, 52)
#define C_WHITE   GFX_RGB(255, 255, 255)   /* P3.3 插入光标：白色 */
#define C_TRANSP  0xFFFFFFFFu

static char G_hist[GT_HIST];
static int  G_len;
static volatile int G_on;
static int  G_dirty;                  /* P3.3: 0 无 / 1 仅输入行 / 2 整窗口 */
static int  G_scroll;                 /* 向上翻了多少行（0 = 贴底） */
static char G_line[GT_LINE];
static int  G_llen;

/* ---- P3.3 白色闪烁插入光标（Linux / Windows 终端节奏）----------------------
 * 打字时光标常亮；停手 500ms 后开始 500ms 亮 / 500ms 灭地闪。
 * 相位由桌面循环喂进来的「真实毫秒」驱动，与 CPU 主频、帧率都无关；
 * 空闲时靠 60fps 帧刷新，不需要给光标单独开一个中断。
 */
#define GT_BLINK_HOLD_MS 500ull
#define GT_BLINK_HALF_MS 500ull
static int  G_blink_on = 1;
static unsigned long long G_last_key_ms;
static int  G_key_kick = 1;           /* 首次进桌面先点亮 */
static struct gfx_rect G_inrect;      /* draw 时缓存：闪烁只脏这一条输入行 */
static int  G_inrect_valid;

struct gt_row { int s, e; };          /* 历史里一行的 [s,e)，不含 \n */

/* ---- 小工具（freestanding，不用 libc） ---- */
static void gt_puts(const char *s)
{
    while (*s) gfxterm_putc(*s++);
}
static int gt_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int gt_nrow(void)
{
    int n = 0;
    for (int i = 0; i < G_len; i++) if (G_hist[i] == '\n') n++;
    if (G_len > 0 && G_hist[G_len - 1] != '\n') n++;
    return n;
}
/* 脏区分级：历史输出变了要重画整个窗口；只有输入行/光标变了，
   就只脏那条 22px 高的输入行 —— 每秒两次闪烁不再重绘 64 行历史。 */
static void gt_mark_all(void)   { G_dirty = 2; }
static void gt_mark_input(void) { if (G_dirty < 1) G_dirty = 1; }

/* ---- 输出挂钩 ---- */
void gfxterm_putc(char c)
{
    if (!G_on) return;
    if (c == '\r') return;            /* uart_puts 为 \n 补的回车不要 */
    if (G_len >= GT_HIST) {
        /* 满：丢掉最老的四分之一，保持尾部连续（不搞环形，省掉取模与换行的复杂度） */
        int drop = GT_HIST / 4;
        for (int i = drop; i < G_len; i++) G_hist[i - drop] = G_hist[i];
        G_len -= drop;
    }
    G_hist[G_len++] = c;
    gt_mark_all();
}

void gfxterm_init(void)
{
    G_len = 0; G_scroll = 0;
    G_llen = 0; G_line[0] = 0;
    G_on = 1;
    G_blink_on = 1; G_key_kick = 1; G_inrect_valid = 0;
    gt_mark_all();
    gt_puts("LoongOS 图形终端  |  输入 help 看命令清单\n");
    gt_puts("提示：开始菜单列出全部 App；Ctrl + Shift + F1 退出桌面\n");
    gt_puts("退出桌面：Ctrl + Shift + F1\n\n");
}
void gfxterm_stop(void) { G_on = 0; }
int  gfxterm_dirty(void) { int d = G_dirty; G_dirty = 0; return d; }

/* P3.3：桌面主循环每个空闲帧喂一次 now_ms（由架构时钟换算）。 */
void gfxterm_blink(unsigned long long now_ms)
{
    if (G_key_kick) {
        G_key_kick = 0;
        G_last_key_ms = now_ms;
        if (!G_blink_on) { G_blink_on = 1; gt_mark_input(); }
        return;
    }
    unsigned long long since = now_ms - G_last_key_ms;
    int on = (since < GT_BLINK_HOLD_MS)
                 ? 1
                 : (((since - GT_BLINK_HOLD_MS) / GT_BLINK_HALF_MS) & 1) == 0;
    if (on != G_blink_on) { G_blink_on = on; gt_mark_input(); }
}

struct gfx_rect gfxterm_input_rect(void)
{
    if (!G_inrect_valid) return (struct gfx_rect){0, 0, 0, 0};
    return G_inrect;
}

void gfxterm_scroll(int delta)
{
    int max = gt_nrow() - 8;
    if (max < 0) max = 0;
    G_scroll += delta;
    if (G_scroll < 0) G_scroll = 0;
    if (G_scroll > max) G_scroll = max;
    gt_mark_all();
}

/* ---- 命令执行 ---- */
void gfxterm_exec(const char *line)
{
    if (!line || !*line) return;
    /* 回显（直接写历史，不再经 uart_putc，避免串口出现重复行） */
    gt_puts("> ");
    gt_puts(line);
    gfxterm_putc('\n');
    G_scroll = 0;                     /* 执行新命令自动贴底 */

    /* 已在桌面里再敲 desktop 会嵌套进入桌面循环，直接拦掉 */
    if (gt_eq(line, "desktop") || gt_eq(line, "desktop ")) {
        gt_puts("已在图形桌面中。按 Ctrl+Shift+F1 返回串口终端。\n");
        return;
    }

    char buf[GT_LINE];
    int i = 0;
    while (line[i] && i < GT_LINE - 1) { buf[i] = line[i]; i++; }
    buf[i] = 0;
    /* shell_dispatch_line 会原地改字符串（管道分割写 0），所以传可写副本 */
    shell_dispatch_line(buf);
    gt_mark_all();
}

/* ---- 输入行编辑 ---- */
void gfxterm_key(char c)
{
    unsigned char uc = (unsigned char)c;
    /* P3.3「正在输入」：任何按键都让光标立刻回到常亮，并从这一刻重新计时。
       不等下一个空闲帧，避免打字时看到光标忽明忽暗。 */
    G_key_kick = 1;
    if (!G_blink_on) G_blink_on = 1;
    gt_mark_input();
    if (c == '\r' || c == '\n') {
        G_line[G_llen] = 0;
        int n = G_llen;
        G_llen = 0;
        if (n) gfxterm_exec(G_line);
        else   { gfxterm_putc('\n'); gt_mark_input(); }
        G_line[0] = 0;
        return;
    }
    if (c == '\b' || c == 127) {
        if (G_llen > 0) {
            /* UTF-8 按「字符」退格：末尾续字节一路回退到起始字节 */
            int n = 1;
            while (n < 4 && n < G_llen &&
                   ((unsigned char)G_line[G_llen - n] & 0xC0) == 0x80) n++;
            G_llen -= n;
        }
        G_line[G_llen] = 0;
        gt_mark_input();
        return;
    }
    if (c == 27) {                    /* Esc 清空当前输入行（不再是退出键） */
        G_llen = 0; G_line[0] = 0; gt_mark_input(); return;
    }
    if ((uc >= 32 && uc < 127) || uc >= 0x80) {
        if (G_llen < GT_LINE - 1) {
            G_line[G_llen++] = c;
            G_line[G_llen] = 0;
            gt_mark_input();
        }
    }
}

/* ---- 绘制 ---- */
void gfxterm_draw(struct gfx_rect r, int focused)
{
    gfx_fill(r, C_BG);
    gfx_clip_push(r);

    int nlines = (r.h - INPUT_H) / LH;
    if (nlines < 1) nlines = 1;
    if (nlines > GT_MAXROW) nlines = GT_MAXROW;

    /* 从尾部往前切行：rows[0] = 最新一行 */
    struct gt_row rows[GT_MAXROW];
    int nrow = 0;
    int e = G_len;
    for (int i = G_len - 1; i >= 0 && nrow < GT_MAXROW; i--) {
        if (G_hist[i] == '\n') {
            rows[nrow].s = i + 1; rows[nrow].e = e; nrow++;
            e = i;
        }
    }
    if (e > 0 && nrow < GT_MAXROW) { rows[nrow].s = 0; rows[nrow].e = e; nrow++; }

    int avail = nrow - G_scroll;
    if (avail < 0) avail = 0;
    int n = avail < nlines ? avail : nlines;
    char buf[GT_ROWBUF];
    for (int k = 0; k < n; k++) {
        struct gt_row *rw = &rows[G_scroll + n - 1 - k];   /* 较老 → 最新 */
        int len = rw->e - rw->s;
        if (len > GT_ROWBUF - 1) len = GT_ROWBUF - 1;
        for (int i = 0; i < len; i++) buf[i] = G_hist[rw->s + i];
        buf[len] = 0;
        gfx_text(r.x + 6, r.y + (nlines - n + k) * LH, buf, C_TEXT, C_TRANSP);
    }

    /* 输入行：贴内容区底部。缓存矩形，闪烁相位切换时只脏这一条 22px 高区域。 */
    int iy = r.y + r.h - INPUT_H;
    G_inrect = (struct gfx_rect){r.x, iy, r.w, INPUT_H};
    G_inrect_valid = 1;
    gfx_fill(G_inrect, C_INPUTBG);
    gfx_fill((struct gfx_rect){r.x, iy, r.w, 1}, GFX_RGB(40, 62, 92));
    gfx_text(r.x + 6, iy + 2, "> ", C_PROMPT, C_TRANSP);
    int tx = r.x + 6 + gfx_text_w("> ");
    gfx_text(tx, iy + 2, G_line, C_TEXT, C_TRANSP);
    int cx = tx + gfx_text_w(G_line);

    /* P3.3 白色闪烁插入光标：只画在获得焦点的终端里，避免多个窗口都亮着。
       灭态画成输入行底色（等于擦掉），不再是旧的青绿/灰色实心方块。 */
    if (focused)
        gfx_draw_caret(cx, iy + 2, G_blink_on, C_WHITE, C_INPUTBG);
    else
        gfx_fill((struct gfx_rect){cx, iy + 2, 8, 16}, C_INPUTBG);

    /* 翻页提示 */
    if (G_scroll > 0)
        gfx_text(r.x + r.w - 96, r.y + 2, "已向上翻页", C_DIM, C_TRANSP);
    gfx_clip_pop();
}
