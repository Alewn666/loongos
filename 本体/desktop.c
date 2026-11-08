/* LoongOS 桌面路线 P2 · 桌面外壳（desktop.c）
 *
 * 用户要求（2026-11-08）：
 *   1) 保留喜欢的 P1 页面风格；
 *   2) 能用鼠标操作（点击 / 拖拽窗口 / 开始菜单 / 按钮）；
 *   3) 在图形页面上能操作电脑的**所有功能**，包括安装系统；
 *   4) 退出桌面必须按 Ctrl+Shift+F1（Q / Esc 不再是退出键）。
 *
 * 实现要点：
 *   - 鼠标：usb_kbd.c 枚举 USB 相对鼠标 + 绝对平板，kernel.c 汇总成 (x,y,btn,wheel)。
 *     本文件每轮取快照，用 gfx_pick_top() 做命中、按 z 序分派点击、拖标题栏移动窗口。
 *   - 「所有功能」：新增终端窗口，键盘输入与开始菜单的快捷命令都走 gfxterm_exec()
 *     → kernel.c 的 shell_dispatch_line()，所以 help / ls / net / install yes / 管道 /
 *     重定向等全部命令都能在图形界面里跑，输出显示在窗口内（uart_putc 挂钩）。
 *   - 安装系统：开始菜单 + 独立「安装系统」窗口，二次确认后执行 install yes。
 *   - 退出：usb_kbd.c 在 HID 报告层识别 Ctrl+Shift+F1 并置标志，这里每轮检查；
 *     串口字符仍自动让位（不消费），保证全套自动化回归不受影响。
 *
 * 预算：静态状态为窗口池 + 按钮表 + 少量标量（< 2KB），不引 kmalloc。
 * 绘图复用 gfx.c 的裁剪栈 / 脏矩形；字形走 kernel.c 的 fb_query_glyph()。
 */
#include <stdint.h>
#include "gfx.h"
#include "gfx_draw.h"
#include "desktop.h"
#include "gfxterm.h"

typedef unsigned long u64;
typedef unsigned int  u32;

/* ---- kernel.c / 驱动提供的桥 ---- */
extern void usb_kbd_poll(void);
extern void net_poll(void);
extern void smp_tick_inc(void);
extern void kputs(const char *s);
#ifdef DESKTOP_MOUSE_DEBUG
extern void kputdec(u32 v);          /* 仅诊断构建用：逐帧打印鼠标状态 */
#endif
extern void fb_console_reset(void);
extern int  desktop_ring_peek_uart(void);
extern int  desktop_ring_pop_kbd(char *c);
extern volatile u64 tick;
extern int  mouse_get(int *x, int *y, int *btn, int *wheel);
extern void mouse_reset(int x, int y);
extern int  desktop_exit_requested(void);

/* ---- 配色 ---- */
#define C_WALL_A    GFX_RGB(16, 30, 52)
#define C_WALL_B    GFX_RGB(34, 66, 104)
#define C_DECO      GFX_RGB(48, 88, 130)
#define C_TOPBAR    GFX_RGB(10, 19, 33)
#define C_TASKBAR   GFX_RGB(12, 23, 40)
#define C_TB_LINE   GFX_RGB(56, 92, 140)
#define C_WIN_BG    GFX_RGB(244, 247, 252)
#define C_WIN_TEXT  GFX_RGB(28, 40, 58)
#define C_WIN_DIM   GFX_RGB(104, 120, 142)
#define C_BORDER    GFX_RGB(148, 168, 194)
#define C_SHADOW    GFX_RGB(8, 16, 28)
#define C_TITLE_DIM GFX_RGB(116, 138, 170)
#define C_BLUE      GFX_RGB(45, 108, 210)
#define C_TEAL      GFX_RGB(38, 168, 142)
#define C_VIOLET    GFX_RGB(122, 92, 196)
#define C_ORANGE    GFX_RGB(214, 138, 52)
#define C_RED       GFX_RGB(206, 72, 72)
#define C_ACCENT    GFX_RGB(86, 214, 176)
#define C_WHITE     GFX_RGB(255, 255, 255)
#define C_BTN       GFX_RGB(52, 96, 158)
#define C_BTN_HI    GFX_RGB(74, 132, 208)
#define C_MENU_BG   GFX_RGB(18, 30, 50)
#define C_TRANSP    0xFFFFFFFFu

#define TOPBAR_H   38
#define TASKBAR_H  42
#define TITLE_H    28
#define STEP       16
#define CURSOR_W   12
#define CURSOR_H   16

/* ---- 窗口 ---- */
enum { W_TERM = 0, W_INSTALL, W_INFO, W_GFX, W_HELP, NWIN };
enum { K_TEXT = 0, K_TERM, K_INSTALL };

struct dwin {
    struct gfx_rect r;
    int z;
    const char *title;
    int kind;
    u32 accent;
    const char *l1, *l2, *l3, *l4;
    int visible;
};

static struct dwin G_win[NWIN];
static int G_focus;
static int G_inited;
static u64 G_last_sec;

/* ---- 鼠标状态 ---- */
static int G_mx, G_my, G_mprev;
static int G_drag = -1;                 /* 正在拖动的窗口索引 */
static int G_drag_dx, G_drag_dy;
static int G_menu_on;
static int G_install_armed;
static u64 G_arm_tick;

/* ---- 按钮 ---- */
enum { ACT_EXEC, ACT_FOCUS, ACT_INSTALL, ACT_CLEAR, ACT_CLOSE, ACT_SCROLL };
#define BT_MENU  (-1)
struct btn {
    int owner;                          /* BT_MENU 或窗口索引 */
    int dx, dy, w, h;                   /* 相对 owner 左上角 */
    const char *label;
    int act;
    int param;                          /* ACT_FOCUS 的窗口索引 */
    const char *arg;                    /* ACT_EXEC 的命令 */
};

#define MENU_W   220
#define MENU_ROW 26
#define MENU_N   10
#define MENU_H   (8 + MENU_N * MENU_ROW + 8)

static const struct btn G_btns[] = {
    /* 开始菜单：常用功能都有图形入口，全部命令仍可在终端里敲 */
    { BT_MENU,   8,   8 + 0*MENU_ROW, 204, 22, "图形终端",   ACT_FOCUS,   W_TERM,    0 },
    { BT_MENU,   8,   8 + 1*MENU_ROW, 204, 22, "安装系统",   ACT_FOCUS,   W_INSTALL, 0 },
    { BT_MENU,   8,   8 + 2*MENU_ROW, 204, 22, "系统信息",   ACT_FOCUS,   W_INFO,    0 },
    { BT_MENU,   8,   8 + 3*MENU_ROW, 204, 22, "操作说明",   ACT_FOCUS,   W_HELP,    0 },
    { BT_MENU,   8,   8 + 4*MENU_ROW, 204, 22, "命令清单",   ACT_EXEC,    0, "help" },
    { BT_MENU,   8,   8 + 5*MENU_ROW, 204, 22, "查看磁盘",   ACT_EXEC,    0, "disks" },
    { BT_MENU,   8,   8 + 6*MENU_ROW, 204, 22, "连接网络",   ACT_EXEC,    0, "net dhcp" },
    { BT_MENU,   8,   8 + 7*MENU_ROW, 204, 22, "网络状态",   ACT_EXEC,    0, "ifconfig" },
    { BT_MENU,   8,   8 + 8*MENU_ROW, 204, 22, "磁盘用量",   ACT_EXEC,    0, "df" },
    { BT_MENU,   8,   8 + 9*MENU_ROW, 204, 22, "清空终端",   ACT_CLEAR,   0, 0 },
    /* 安装系统窗口内 */
    { W_INSTALL, 14,  88, 162, 26, "检查磁盘",   ACT_EXEC,    0, "disks" },
    { W_INSTALL, 14, 122, 162, 26, "一键安装",   ACT_INSTALL, 0, 0 },
    /* 系统信息窗口内 */
    { W_INFO,    14, 108, 162, 26, "刷新状态",   ACT_EXEC,    0, "df" },
};
#define NBTN ((int)(sizeof G_btns / sizeof G_btns[0]))

/* ---- 小工具 ---- */
static u32 mix(u32 a, u32 b, int t)
{
    u32 ar = (a >> 16) & 255u, ag = (a >> 8) & 255u, ab = a & 255u;
    u32 br = (b >> 16) & 255u, bg = (b >> 8) & 255u, bb = b & 255u;
    u32 r  = (ar * (255u - (u32)t) + br * (u32)t) / 255u;
    u32 g  = (ag * (255u - (u32)t) + bg * (u32)t) / 255u;
    u32 bl = (ab * (255u - (u32)t) + bb * (u32)t) / 255u;
    return GFX_RGB(r, g, bl);
}
static void fmt2(char *p, int v)
{
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}
static void fmt_time(char *b, u64 sec)
{
    const char *pre = "已运行 ";
    int i = 0;
    while (*pre) b[i++] = *pre++;
    int m = (int)(sec / 60) % 100, s = (int)(sec % 60);
    fmt2(b + i, m); i += 2;
    b[i++] = ':';
    fmt2(b + i, s); i += 2;
    b[i] = 0;
}
static struct gfx_rect full_screen(void)
{
    return (struct gfx_rect){0, 0, gfx_width(), gfx_height()};
}
static struct gfx_rect clock_rect(void)
{
    return (struct gfx_rect){gfx_width() - 196, gfx_height() - TASKBAR_H,
                             196, TASKBAR_H};
}
static struct gfx_rect start_rect(void)
{
    return (struct gfx_rect){10, gfx_height() - TASKBAR_H + 8, 78, 26};
}
static struct gfx_rect menu_rect(void)
{
    return (struct gfx_rect){10, gfx_height() - TASKBAR_H - MENU_H,
                             MENU_W, MENU_H};
}
static struct gfx_rect taskbtn_rect(int i)
{
    return (struct gfx_rect){96 + i * 96, gfx_height() - TASKBAR_H + 8, 90, 26};
}
static struct gfx_rect cursor_rect(int x, int y)
{
    return (struct gfx_rect){x, y, CURSOR_W + 2, CURSOR_H + 2};
}
static struct gfx_rect close_rect(struct gfx_rect r)
{
    return (struct gfx_rect){r.x + r.w - 24, r.y + 4, 20, 22};
}
static struct gfx_rect btn_rect(const struct btn *b)
{
    struct gfx_rect r;
    if (b->owner == BT_MENU) {
        struct gfx_rect m = menu_rect();
        r = (struct gfx_rect){m.x + b->dx, m.y + b->dy, b->w, b->h};
    } else {
        struct gfx_rect w = G_win[b->owner].r;
        r = (struct gfx_rect){w.x + b->dx, w.y + b->dy, b->w, b->h};
    }
    return r;
}
static int btn_visible(const struct btn *b)
{
    return (b->owner == BT_MENU) ? G_menu_on : G_win[b->owner].visible;
}

/* ---- 窗口池 ---- */
static void clamp_win(struct gfx_rect *r)
{
    int w = gfx_width(), h = gfx_height();
    int y0 = TOPBAR_H + 4, y1 = h - TASKBAR_H - 4;
    if (w < 160 || y1 - y0 < 100) return;
    if (r->w > w - 16)  r->w = w - 16;
    if (r->h > y1 - y0) r->h = y1 - y0;
    if (r->w < 120) r->w = 120;
    if (r->h < 96)  r->h = 96;
    if (r->x < 4)             r->x = 4;
    if (r->x > w - 4 - r->w)  r->x = w - 4 - r->w;
    if (r->y < y0)            r->y = y0;
    if (r->y > y1 - r->h)     r->y = y1 - r->h;
}

static void wins_init(void)
{
    int w = gfx_width(), h = gfx_height();
    int y0 = TOPBAR_H + 6, y1 = h - TASKBAR_H - 6;
    int ah = y1 - y0;
    int lw = (w - 24 * 2 - 12) * 68 / 100;
    int rx = 24 + lw + 12;
    int rw = w - rx - 24;
    int th = ah * 66 / 100;
    int bh = ah - th - 10;

    G_win[W_TERM] = (struct dwin){
        {24, y0, lw, th}, 10, "图形终端", K_TERM, C_BLUE,
        0, 0, 0, 0, 1 };
    G_win[W_INSTALL] = (struct dwin){
        {rx, y0, rw, th / 2}, 9, "安装系统", K_INSTALL, C_ORANGE,
        0, 0, 0, 0, 1 };
    G_win[W_INFO] = (struct dwin){
        {rx, y0 + th / 2 + 10, rw, th / 2}, 8, "系统信息", K_TEXT, C_TEAL,
        "内核  LoongOS AArch64", "屏幕  32bpp 图形界面",
        "鼠标  点击 / 拖动窗口", "终端  可执行全部命令", 1 };
    G_win[W_GFX] = (struct dwin){
        {24, y0 + th + 10, lw / 2 - 6, bh}, 7, "图形窗口", K_TEXT, C_VIOLET,
        "裁剪 / 命中 / z 序", "脏矩形局部重绘",
        "按 W A S D 移动窗口", "按 1 2 3 4 5 选中", 1 };
    G_win[W_HELP] = (struct dwin){
        {24 + lw / 2 + 6, y0 + th + 10, lw / 2 - 6, bh}, 6, "操作说明", K_TEXT, C_TEAL,
        "鼠标左键  点击 / 拖动", "开始菜单  全部功能入口",
        "终端窗口  直接敲命令", "Ctrl+Shift+F1  返回终端", 1 };

    for (int i = 0; i < NWIN; i++) clamp_win(&G_win[i].r);
    G_focus = W_TERM;
    G_last_sec = (u64)-1;
    G_menu_on = 0;
    G_install_armed = 0;
    G_drag = -1;
    G_inited = 1;
}

static void raise_focus(void)
{
    struct gfx_win zs[NWIN];
    for (int i = 0; i < NWIN; i++) { zs[i].r = G_win[i].r; zs[i].z = G_win[i].z; }
    G_win[G_focus].z = gfx_top_z(zs, NWIN);
    gfx_damage(full_screen());
}
static void focus_win(int i)
{
    if (i < 0 || i >= NWIN) return;
    G_win[i].visible = 1;
    G_focus = i;
    raise_focus();
}
static void move_focus(int dx, int dy)
{
    G_win[G_focus].r.x += dx;
    G_win[G_focus].r.y += dy;
    clamp_win(&G_win[G_focus].r);
    gfx_damage(full_screen());      /* 整屏重画，杜绝移动残影（P1 实测结论） */
}
static int top_win_at(int x, int y)
{
    struct gfx_win zs[NWIN];
    for (int i = 0; i < NWIN; i++) {
        zs[i].r = G_win[i].r;
        zs[i].z = G_win[i].visible ? G_win[i].z : -1000;
    }
    return gfx_pick_top(zs, NWIN, x, y);
}

/* ---- 动作 ---- */
static void do_action(const struct btn *b)
{
    switch (b->act) {
    case ACT_EXEC:
        gfxterm_exec(b->arg);
        focus_win(W_TERM);
        break;
    case ACT_FOCUS:
        focus_win(b->param);
        break;
    case ACT_INSTALL:
        if (!G_install_armed) {
            G_install_armed = 1;
            G_arm_tick = tick;
            gfxterm_exec("disks");          /* 先看一眼目标盘再决定 */
            gfx_damage(full_screen());      /* 按钮变「确认」+ 警告文字 */
        } else {
            G_install_armed = 0;
            gfxterm_exec("install yes");    /* 真正装盘，输出在终端里 */
            focus_win(W_TERM);
        }
        break;
    case ACT_CLEAR:
        gfxterm_init();                     /* 重新初始化 = 清空历史 */
        focus_win(W_TERM);
        break;
    default: break;
    }
}

/* ---- 鼠标事件 ---- */
static void handle_press(void)
{
    int x = G_mx, y = G_my;

    /* 1. 任务栏：开始按钮 */
    if (gfx_hit(start_rect(), x, y)) {
        G_menu_on = !G_menu_on;
        gfx_damage(full_screen());
        return;
    }
    /* 2. 任务栏：窗口按钮 */
    for (int i = 0; i < NWIN; i++) {
        if (gfx_hit(taskbtn_rect(i), x, y)) { focus_win(i); G_menu_on = 0; return; }
    }
    /* 3. 开始菜单（打开时优先吃点击） */
    if (G_menu_on) {
        for (int i = 0; i < NBTN; i++) {
            const struct btn *b = &G_btns[i];
            if (b->owner != BT_MENU) continue;
            if (gfx_hit(btn_rect(b), x, y)) {
                G_menu_on = 0;
                do_action(b);
                return;
            }
        }
        if (!gfx_hit(menu_rect(), x, y)) {
            G_menu_on = 0;
            gfx_damage(full_screen());
        }
        return;
    }
    /* 4. 最顶层命中的窗口 */
    int wi = top_win_at(x, y);
    if (wi < 0) return;
    struct gfx_rect r = G_win[wi].r;

    /* 4a. 标题栏关闭按钮 */
    if (gfx_hit(close_rect(r), x, y) && y < r.y + TITLE_H) {
        G_win[wi].visible = 0;
        if (G_focus == wi) G_focus = W_TERM;
        gfx_damage(full_screen());
        return;
    }
    /* 4b. 窗口内按钮 */
    for (int i = 0; i < NBTN; i++) {
        const struct btn *b = &G_btns[i];
        if (b->owner != wi || !btn_visible(b)) continue;
        if (gfx_hit(btn_rect(b), x, y)) { do_action(b); return; }
    }
    /* 4c. 标题栏 → 拖动；内容区 → 只聚焦 */
    focus_win(wi);
    if (y < r.y + TITLE_H) {
        G_drag = wi;
        G_drag_dx = x - r.x;
        G_drag_dy = y - r.y;
    }
}

static void poll_mouse(void)
{
    int nx, ny, nb, wh;
    if (!mouse_get(&nx, &ny, &nb, &wh)) return;

#ifdef DESKTOP_MOUSE_DEBUG
    kputs("[mdbg] x="); kputdec((u32)nx);
    kputs(" y="); kputdec((u32)ny);
    kputs(" btn="); kputdec((u32)nb);
    kputs(" prev="); kputdec((u32)G_mprev);
    kputs(" drag="); kputdec((u32)(G_drag + 1));
    kputs("\n");
#endif

    struct gfx_rect old = cursor_rect(G_mx, G_my);
    G_mx = nx; G_my = ny;
    struct gfx_rect now = cursor_rect(G_mx, G_my);
    gfx_damage(gfx_union(old, now));       /* 光标只脏两个小矩形 */

    int pressed  = (nb & 1) && !(G_mprev & 1);
    int released = !(nb & 1) && (G_mprev & 1);
    if (wh) {
        gfxterm_scroll(wh > 0 ? 2 : -2);
        gfx_damage(G_win[W_TERM].r);
    }
    if (pressed)  handle_press();
    if (released) G_drag = -1;
    if (G_drag >= 0) {
        struct gfx_rect *r = &G_win[G_drag].r;
        r->x = G_mx - G_drag_dx;
        r->y = G_my - G_drag_dy;
        clamp_win(r);
        gfx_damage(full_screen());
    }
    G_mprev = nb;
}

/* ---- 键盘 ---- */
static void handle_key(char c)
{
    if (c == '\t' || c == 'n' || c == 'N') {
        int n = G_focus;
        for (int k = 1; k <= NWIN; k++) {
            int i = (n + k) % NWIN;
            if (G_win[i].visible) { focus_win(i); break; }
        }
        return;
    }
    if (c >= '1' && c <= '5') { focus_win(c - '1'); return; }
    if (c == 'w' || c == 'W') { move_focus(0, -STEP); return; }
    if (c == 's' || c == 'S') { move_focus(0,  STEP); return; }
    if (c == 'a' || c == 'A') { move_focus(-STEP, 0); return; }
    if (c == 'd' || c == 'D') { move_focus( STEP, 0); return; }
}

/* ---- 各层绘制 ---- */
static void draw_wallpaper(void)
{
    int w = gfx_width(), h = gfx_height();
    for (int y = 0; y < h; y++)
        gfx_fill((struct gfx_rect){0, y, w, 1},
                 mix(C_WALL_A, C_WALL_B, h > 1 ? (y * 255) / (h - 1) : 0));
    for (int i = 0; i < 7; i++)
        gfx_line(w - 260 + i * 22, h - TASKBAR_H, w - 90 + i * 22, TOPBAR_H + 56,
                 C_DECO);
}
static void draw_topbar(void)
{
    int w = gfx_width();
    gfx_fill((struct gfx_rect){0, 0, w, TOPBAR_H}, C_TOPBAR);
    gfx_fill((struct gfx_rect){0, TOPBAR_H - 2, w, 2}, C_BLUE);
    gfx_text(16, 11, "灵珑系统  LoongOS", C_WHITE, C_TRANSP);
    const char *hint = "图形界面 P2   |   Ctrl+Shift+F1 返回终端";
    int hw = gfx_text_w(hint);
    if (w - hw - 16 > 260)
        gfx_text(w - hw - 16, 11, hint, C_ACCENT, C_TRANSP);
}

static void draw_button_rect(struct gfx_rect r, const char *label,
                             u32 bg, u32 fg)
{
    gfx_fill(r, bg);
    gfx_rect_draw(r, GFX_RGB(90, 120, 160), 1);
    int tw = gfx_text_w(label);
    gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 16) / 2, label, fg, C_TRANSP);
}

static void draw_one_win(const struct dwin *w, int focused, int idx)
{
    struct gfx_rect r = w->r;
    gfx_fill((struct gfx_rect){r.x + 4, r.y + 4, r.w, r.h}, C_SHADOW);
    gfx_fill(r, C_WIN_BG);
    gfx_rect_draw(r, C_BORDER, 1);
    gfx_fill((struct gfx_rect){r.x, r.y, r.w, TITLE_H},
             focused ? w->accent : C_TITLE_DIM);
    /* 标题栏关闭按钮 */
    struct gfx_rect cr = close_rect(r);
    gfx_fill((struct gfx_rect){cr.x + 3, cr.y + 6, 14, 12},
             focused ? C_WHITE : C_WIN_BG);
    gfx_text(cr.x + 5, cr.y + 4, "X", focused ? w->accent : C_TITLE_DIM, C_TRANSP);
    gfx_text(r.x + 12, r.y + 6, w->title, C_WHITE, C_TRANSP);

    struct gfx_rect content = (struct gfx_rect){r.x + 1, r.y + TITLE_H,
                                                r.w - 2, r.h - TITLE_H - 1};
    if (w->kind == K_TERM) {
        gfxterm_draw(content, focused);
        return;
    }
    gfx_clip_push(content);
    gfx_fill((struct gfx_rect){r.x + 1, r.y + TITLE_H, r.w - 2, 2}, w->accent);
    if (w->kind == K_INSTALL) {
        gfx_text(r.x + 14, r.y + TITLE_H + 10, "把系统装到本机硬盘", C_WIN_TEXT, C_TRANSP);
        gfx_text(r.x + 14, r.y + TITLE_H + 32, "会清空目标磁盘", C_WIN_DIM, C_TRANSP);
        if (G_install_armed)
            gfx_text(r.x + 14, r.y + TITLE_H + 56, "再点一次确认安装", C_RED, C_TRANSP);
        else
            gfx_text(r.x + 14, r.y + TITLE_H + 56, "先点检查磁盘", C_WIN_DIM, C_TRANSP);
    } else {
        gfx_text(r.x + 16, r.y + TITLE_H + 8,  w->l1, C_WIN_TEXT, C_TRANSP);
        gfx_text(r.x + 16, r.y + TITLE_H + 26, w->l2, C_WIN_TEXT, C_TRANSP);
        gfx_text(r.x + 16, r.y + TITLE_H + 44, w->l3, C_WIN_DIM,  C_TRANSP);
        gfx_text(r.x + 16, r.y + TITLE_H + 62, w->l4, C_WIN_DIM,  C_TRANSP);
    }
    /* 窗口内按钮 */
    for (int i = 0; i < NBTN; i++) {
        const struct btn *b = &G_btns[i];
        if (b->owner != idx) continue;
        const char *label = b->label;
        u32 bg = C_BTN;
        if (b->act == ACT_INSTALL) {
            if (G_install_armed) { label = "确认清空并安装"; bg = C_RED; }
            else                 { label = "一键安装"; }
        }
        struct gfx_rect br = btn_rect(b);
        if (gfx_hit(br, G_mx, G_my)) bg = C_BTN_HI;
        draw_button_rect(br, label, bg, C_WHITE);
    }
    gfx_clip_pop();
}

static void draw_windows(void)
{
    int order[NWIN] = {0, 1, 2, 3, 4};
    for (int i = 0; i < NWIN; i++)
        for (int j = i + 1; j < NWIN; j++)
            if (G_win[order[j]].z < G_win[order[i]].z) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
    for (int i = 0; i < NWIN; i++) {
        int k = order[i];
        if (G_win[k].visible) draw_one_win(&G_win[k], k == G_focus, k);
    }
}

static void draw_taskbar(void)
{
    int w = gfx_width(), h = gfx_height();
    int ty = h - TASKBAR_H;
    gfx_fill((struct gfx_rect){0, ty, w, TASKBAR_H}, C_TASKBAR);
    gfx_fill((struct gfx_rect){0, ty, w, 1}, C_TB_LINE);
    struct gfx_rect sr = start_rect();
    draw_button_rect(sr, "开始", G_menu_on ? C_TEAL : C_BLUE, C_WHITE);
    for (int i = 0; i < NWIN; i++) {
        struct gfx_rect br = taskbtn_rect(i);
        u32 bg = (i == G_focus) ? C_BTN_HI : GFX_RGB(28, 44, 70);
        draw_button_rect(br, G_win[i].title, bg,
                         G_win[i].visible ? C_WHITE : C_WIN_DIM);
    }
    char tb[32];
    fmt_time(tb, tick / 10);
    gfx_text(w - gfx_text_w(tb) - 16, ty + 13, tb, C_ACCENT, C_TRANSP);
}

static void draw_menu(void)
{
    if (!G_menu_on) return;
    struct gfx_rect m = menu_rect();
    gfx_fill((struct gfx_rect){m.x + 3, m.y + 3, m.w, m.h}, C_SHADOW);
    gfx_fill(m, C_MENU_BG);
    gfx_rect_draw(m, C_TB_LINE, 1);
    gfx_fill((struct gfx_rect){m.x, m.y, m.w, 2}, C_TEAL);
    for (int i = 0; i < NBTN; i++) {
        const struct btn *b = &G_btns[i];
        if (b->owner != BT_MENU) continue;
        struct gfx_rect br = btn_rect(b);
        u32 bg = gfx_hit(br, G_mx, G_my) ? C_BTN_HI : C_BTN;
        draw_button_rect(br, b->label, bg, C_WHITE);
    }
}

/* 光标：白箭头 + 深色描边。redraw 末尾调用，受裁剪栈限制 —— 只有脏区
   覆盖光标当前位置时才会被画出来；旧位置重画时背景会把它擦掉。 */
static void draw_cursor(void)
{
    for (int i = 0; i < CURSOR_H; i++) {
        int half = i / 2;
        gfx_fill((struct gfx_rect){G_mx, G_my + i, half + 2, 1},
                 GFX_RGB(8, 12, 20));
    }
    for (int i = 1; i < CURSOR_H - 2; i++) {
        int half = i / 2;
        gfx_fill((struct gfx_rect){G_mx + 1, G_my + i, half, 1}, C_WHITE);
    }
}

static void redraw(struct gfx_rect r)
{
    (void)r;
    draw_wallpaper();
    draw_topbar();
    draw_windows();
    draw_taskbar();
    draw_menu();
    draw_cursor();
}

/* ---- 桌面主循环 ---- */
void desktop_run(void)
{
    if (!gfx_ready()) {
        kputs("[desktop] 无帧缓冲，保持串口 shell\n");
        return;
    }

    gfx_take_screen();                 /* uart_putc 不再把日志镜像到帧缓冲 */
    gfx_set_redraw(redraw);
    if (!G_inited) {
        wins_init();
        mouse_reset(gfx_width() / 2, gfx_height() / 2);
        G_mx = gfx_width() / 2; G_my = gfx_height() / 2;
        G_mprev = 0;
    }
    gfxterm_init();                    /* 每次进入给一份干净的终端历史 */
    fb_console_reset();
    gfx_damage(full_screen());
    gfx_present();
    kputs("[desktop] 图形桌面已就绪：鼠标点击/拖动，开始菜单进入全部功能，终端可跑所有命令\n");
    kputs("[desktop] 退出请按 Ctrl+Shift+F1（串口输入会自动让位）\n");

    for (;;) {
        smp_tick_inc();
        usb_kbd_poll();                /* 键盘 + 鼠标 + 组合键都在这里收割 */
        net_poll();
        __asm__ volatile("wfi");

        /* 串口控制台优先：只要有串口字符就退出桌面，且**不消费**它，
           让 shell_loop() 原样取走。全套自动化回归不受影响。 */
        if (desktop_ring_peek_uart()) break;
        /* 用户指定：只有 Ctrl+Shift+F1 能主动退出桌面 */
        if (desktop_exit_requested()) break;

        char c;
        while (desktop_ring_pop_kbd(&c)) {
            if (G_focus == W_TERM) {
                if (c == '\t') handle_key('\t');   /* Tab 仍可切窗口 */
                else gfxterm_key(c);
            } else {
                handle_key(c);
            }
        }

        poll_mouse();
        if (gfxterm_dirty()) gfx_damage(G_win[W_TERM].r);
        /* 二次确认 10 秒后自动撤销 */
        if (G_install_armed && tick - G_arm_tick > 100) {
            G_install_armed = 0;
            gfx_damage(full_screen());
        }
        u64 sec = tick / 10;
        if (sec != G_last_sec) { G_last_sec = sec; gfx_damage(clock_rect()); }
        gfx_present();
    }

    gfxterm_stop();
    gfx_release_screen();              /* 恢复 uart_putc → fb_putc 的文字镜像 */
    fb_console_reset();
    kputs("[desktop] 已返回串口终端（Ctrl+Shift+F1 可再按一次）\n");
}
