/* LoongOS x86_64 · 图形桌面外壳
 * ----------------------------------------------------------------------------
 * 复用 ARM 版 gfx.c（纯 C 绘图 + 中英混排字库）。本文件只做：
 *   1) 主循环消费 input 核心层的原子帧（Linux 模型：中断上半部入队、这里读帧）；
 *   2) 按帧边界的一致状态重绘光标 / 窗口 / 键盘回显；
 *   3) Ctrl+Shift+F1 退出到串口终端（与 ARM 版一致的约定）。
 *
 * 关键设计：光标位置只读 input_get_pointer() 的「帧边界快照」，绝不自己维护
 * 第二套坐标。旧 ARM 版那些「点一下才知道位置」「按钮位互相覆盖」「漂移」
 * 都源于多套状态和隐式时序，这里从架构上不存在。
 */
#include <stdint.h>
#include "platform.h"
#include "input.h"
#include "gfx_draw.h"
#include "gfx.h"

typedef uint32_t u32;

extern volatile u64 g_ticks;
extern u64 g_mbi_addr;

/* ---- 绘制辅助 ---- */
static void draw_bg(void)
{
    int W = gfx_width(), H = gfx_height();
    for (int y = 0; y < H; y++) {
        /* 竖向渐变：深蓝 → 靛青 */
        u32 r = 12 + (u32)y * 10 / (u32)(H ? H : 1);
        u32 g = 20 + (u32)y * 18 / (u32)(H ? H : 1);
        u32 b = 48 + (u32)y * 40 / (u32)(H ? H : 1);
        gfx_fill((struct gfx_rect){0, y, W, 1}, GFX_RGB(r, g, b));
    }
}

static void draw_window(struct gfx_rect r, const char *title)
{
    /* 阴影 */
    gfx_fill((struct gfx_rect){r.x+3, r.y+3, r.w, r.h}, GFX_RGB(0,0,0));
    /* 窗体 */
    gfx_fill(r, GFX_RGB(0xEE,0xEE,0xF2));
    /* 标题栏 */
    gfx_fill((struct gfx_rect){r.x, r.y, r.w, 22}, GFX_RGB(0x2D,0x5B,0xC4));
    gfx_text(r.x+8, r.y+4, title, GFX_RGB(255,255,255), 0xFFFFFFFFu);
    /* 边框 */
    gfx_rect_draw(r, GFX_RGB(0x99,0x99,0xAA), 1);
}

/* 键盘回显缓冲 */
static char G_line[64];
static int  G_line_n;
static int  G_exit;          /* Ctrl+Shift+F1 退出标志 */

/* ---- P3.3 白色闪烁插入光标（与 ARM 图形终端同一套手感）------------------
 * 用共用绘图层 gfx_draw_caret() 画方块；相位由 PIT g_ticks 驱动（100Hz，
 * 10ms/tick），打字后 500ms 常亮，之后 500ms 亮 / 500ms 灭。
 * 输入行改成深色终端条，白色光标才看得见。
 */
#define XT_INPUT_BG  GFX_RGB(20, 33, 52)
#define XT_TEXT      GFX_RGB(207, 224, 242)
#define XT_CARET     GFX_RGB(255, 255, 255)
static int  G_blink_on = 1;
static unsigned long long G_last_key_ms;
static int  G_key_kick = 1;

/* 记录指针轨迹，画一条淡色轨迹线，直观验证「实时跟随」 */
#define TRAIL_MAX 32
static int G_tx[TRAIL_MAX], G_ty[TRAIL_MAX], G_tn;

static void push_trail(int x, int y)
{
    if (G_tn < TRAIL_MAX) {
        G_tx[G_tn] = x; G_ty[G_tn] = y; G_tn++;
    } else {
        for (int i = 1; i < TRAIL_MAX; i++) { G_tx[i-1]=G_tx[i]; G_ty[i-1]=G_ty[i]; }
        G_tx[TRAIL_MAX-1]=x; G_ty[TRAIL_MAX-1]=y;
    }
}

static void draw_all(void)
{
    int W = gfx_width(), H = gfx_height();
    draw_bg();

    /* 顶栏 */
    gfx_fill((struct gfx_rect){0,0,W,26}, GFX_RGB(0x1A,0x22,0x3A));
    gfx_text(10, 5, "LoongOS x86_64  |  Linux 风格输入子系统",
             GFX_RGB(0xE8,0xEC,0xF6), 0xFFFFFFFFu);

    /* 主窗口：说明 */
    struct gfx_rect w1 = {40, 48, 420, 150};
    draw_window(w1, "桌面已就绪");
    gfx_text(w1.x+12, w1.y+34, "鼠标：移动即可实时跟随（中断驱动 + EV_SYN 帧）",
             GFX_RGB(0x22,0x22,0x2A), 0xFFFFFFFFu);
    gfx_text(w1.x+12, w1.y+56, "键盘：直接打字，支持 Shift / Ctrl / 自动重复",
             GFX_RGB(0x22,0x22,0x2A), 0xFFFFFFFFu);
    gfx_text(w1.x+12, w1.y+78, "退出桌面：Ctrl + Shift + F1",
             GFX_RGB(0xB0,0x30,0x30), 0xFFFFFFFFu);
    gfx_text(w1.x+12, w1.y+100, "相对/绝对统一，按钮边沿上报，无坐标漂移",
             GFX_RGB(0x33,0x66,0x99), 0xFFFFFFFFu);

    /* 终端窗口：深色输入条 + 白色文字 + 白色闪烁插入光标（P3.3） */
    struct gfx_rect w2 = {40, 220, 520, 150};
    draw_window(w2, "图形终端");
    struct gfx_rect inr = {w2.x + 8, w2.y + 30, w2.w - 16, 24};
    gfx_fill(inr, XT_INPUT_BG);
    char prompt[80];
    int n = 0;
    const char *pre = "> ";
    while (pre[n]) { prompt[n] = pre[n]; n++; }
    for (int i = 0; i < G_line_n; i++) prompt[n++] = G_line[i];
    prompt[n] = 0;
    gfx_text(inr.x + 4, inr.y + 4, prompt, XT_TEXT, 0xFFFFFFFFu);
    int caret_x = inr.x + 4 + gfx_text_w(prompt);
    gfx_draw_caret(caret_x, inr.y + 4, G_blink_on, XT_CARET, XT_INPUT_BG);

    /* 状态窗口：诊断数据（帧数 / 指针坐标 / 丢失数 / 节拍） */
    struct gfx_rect w3 = {480, 48, 280, 150};
    draw_window(w3, "输入诊断");
    {
        gfx_text(w3.x+12, w3.y+34, "帧模型: input_event + EV_SYN",
                 GFX_RGB(0x22,0x22,0x2A), 0xFFFFFFFFu);
        gfx_text(w3.x+12, w3.y+56, "驱动: i8042 (IRQ1/IRQ12)",
                 GFX_RGB(0x22,0x22,0x2A), 0xFFFFFFFFu);
        gfx_text(w3.x+12, w3.y+78, "节拍: PIT 100Hz",
                 GFX_RGB(0x22,0x22,0x2A), 0xFFFFFFFFu);
        gfx_text(w3.x+12, w3.y+100, "已消费帧数见串口日志",
                 GFX_RGB(0x66,0x66,0x88), 0xFFFFFFFFu);
    }

    /* 鼠标轨迹 */
    for (int i = 1; i < G_tn; i++)
        gfx_line(G_tx[i-1], G_ty[i-1], G_tx[i], G_ty[i], GFX_RGB(0x00,0xE0,0xE0));

    /* 光标（箭头）：用两条线 + 填充，位置来自帧边界快照 */
    int cx, cy, cb;
    input_get_pointer(&cx, &cy, &cb);
    u32 cc = (cb & 1) ? GFX_RGB(0xFF,0x40,0x40) : GFX_RGB(255,255,255);
    for (int i = 0; i < 12; i++) {
        gfx_line(cx, cy+i, cx+i/2, cy+12, GFX_RGB(0x10,0x10,0x10)); /* 描边 */
    }
    gfx_line(cx, cy, cx, cy+16, cc);
    gfx_line(cx, cy, cx+11, cy+11, cc);
    gfx_line(cx+11, cy+11, cx, cy+16, cc);
    gfx_line(cx, cy, cx+3, cy+12, cc);

    /* 底栏 */
    gfx_fill((struct gfx_rect){0,H-26,W,26}, GFX_RGB(0x1A,0x22,0x3A));
    gfx_text(10, H-21, "LoongOS  |  x86_64  |  Ctrl+Shift+F1 返回终端",
             GFX_RGB(0xC8,0xCC,0xD8), 0xFFFFFFFFu);
}

/* 处理一帧：键盘回显 + 组合键 + 轨迹记录 */
static void handle_frame(const struct input_event *ev, int n)
{
    for (int i = 0; i < n; i++) {
        const struct input_event *e = &ev[i];
        if (e->type != EV_KEY) continue;
        int mods = input_mods();   /* 帧边界后的位图状态 */

        /* Ctrl+Shift+F1 退出桌面（与 ARM 版一致） */
        if (e->code == KEY_F1 && e->value == KEY_VAL_DOWN &&
            (mods & MOD_CTRL) && (mods & MOD_SHIFT)) {
            G_exit = 1;
            continue;
        }
        if (e->value == KEY_VAL_UP) continue;       /* 只在按下/重复时回显 */
        /* P3.3 正在输入：光标立刻回到常亮并从这一刻重新计时 */
        G_key_kick = 1;
        if (!G_blink_on) G_blink_on = 1;
        char c;
        if (input_key_to_ascii(e->code, mods, &c)) {
            if (c == '\b') { if (G_line_n) G_line_n--; }
            else if (c == '\n') { G_line_n = 0; }
            else if (G_line_n < (int)sizeof(G_line)-1) G_line[G_line_n++] = c;
        }
    }
    /* 记录指针轨迹（帧边界快照） */
    int x, y, b; input_get_pointer(&x, &y, &b);
    push_trail(x, y);
}

/* 桌面主循环。返回时表示请求退出到终端。 */
void desktop_run(void)
{
    G_line_n = 0; G_exit = 0; G_tn = 0;
    gfx_take_screen();
    draw_all();

    struct input_event buf[INPUT_FRAME_MAX];
    G_blink_on = 1; G_key_kick = 1;
    for (;;) {
        if (G_exit) break;
        int got = 0;
        while (input_frame_ready()) {
            int n;
            if (input_read_frame(buf, INPUT_FRAME_MAX, &n)) {
                handle_frame(buf, n);
                got = 1;
            }
        }

        /* P3.3 闪烁相位：PIT 100Hz → 10ms/tick。hlt 每 10ms 被 PIT 唤醒，
           空闲时也按真实时间推进，相位翻转就重绘一次（光标闪烁）。 */
        unsigned long long now_ms = g_ticks * 10ull;
        if (G_key_kick) { G_key_kick = 0; G_last_key_ms = now_ms; }
        unsigned long long since = now_ms - G_last_key_ms;
        int on = (since < 500ull) ? 1
               : (((since - 500ull) / 500ull) & 1) == 0;
        if (on != G_blink_on) { G_blink_on = on; got = 1; }

        if (got) draw_all();               /* 有输入帧或相位翻转就重绘 */

        /* 空闲时 hlt 等中断，避免空转烧 CPU；PIT 会周期性唤醒 */
        __asm__ volatile("hlt");
    }
    gfx_release_screen();
}

/* 供 kernel_main 打印状态（不引 stdio） */
void desktop_diag(void)
{
    uart_puts("[desktop] 已退出，消费帧=");
    uart_dec((u32)input_frames_consumed());
    uart_puts(" 丢失="); uart_dec((u32)input_lost());
    uart_puts(" ticks="); uart_dec((u32)g_ticks);
    uart_puts("\n");
}
