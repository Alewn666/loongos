#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
#include "fs.h"        /* 文件系统 API：文件管理器 / 文本编辑器 直接读写 */
/* LoongOS 桌面路线 P4 · GUI 应用生态（desktop.c）
 *
 * 用户诉求（2026-11-11）：
 *   1) 之前那个系统的「开始按钮」根本没用 —— 点开是个空壳菜单，背后没有任何应用。
 *   2) 把 GUI 应用生态做出来。
 *
 * 本轮做法：把「窗口」重新定义为「一个 App 的实例」，所有窗口由统一的 App 注册表驱动：
 *   - struct gapp：每个应用含 name / accent / single（单实例）/ init / draw / press / key / scroll；
 *   - 开始按钮 → 动态开始菜单（列出全部 App + 关机/重启），点应用即开窗口；
 *   - 任务栏：每个打开的窗口一个按钮，可聚焦/还原；窗口数超出时自动均分宽度；
 *   - 多窗口：打开/聚焦/关闭、标题栏拖动、关闭按钮、z 序、脏矩形重绘全部复用既有机制。
 *
 * 应用清单（共 NAPP 个，开始菜单全列）：
 *   终端 / 系统信息 / 图形演示 / 操作说明（原硬编码窗口迁移）
 *   计算器 / 系统监视器 / 设置 / 关于（新增轻量 App）
 *   文件管理器 / 文本编辑器（新增，直接吃 fs.h 的读写 API）
 *
 * 预算：静态状态为窗口池 + App 注册表 + 每 App 小状态（编辑器缓冲 ~1.5KB 为主），
 * 不引 kmalloc。App 缓冲尽量小，避免顶穿 4KB BSS 守卫。
 * 绘图复用 gfx.c 的裁剪栈 / 脏矩形；字形走 kernel.c 的 fb_query_glyph()。
 */
#include <stdint.h>
#include "gfx.h"
#include "gfx_draw.h"
#include "desktop.h"
#include "gfxterm.h"
#include "path.h"      /* 纯函数路径层：修 H1，所有拼接带 cap，见 path.h 注释 */

typedef unsigned long u64;
typedef unsigned int  u32;

/* ---- P3.1 桌面帧时钟 -------------------------------------------------------
 * kernel.c 的全局 tick 是 10Hz：fs 的 mtime、EL0 看门狗、任务心跳、桌面时钟
 * 都写死「1 tick = 100ms」，改频率会破坏一大片语义。但 GUI 需要 60fps 的
 * 呈现节奏 + 零等待的输入收割，两者必须解耦 —— 这里直接读 AArch64 generic
 * timer 的硬件计数器，只给桌面循环用，不动全局 tick。
 */
static inline u64 dsk_cntpct(void)
{
    u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
static inline u64 dsk_cntfrq(void)
{
    u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}

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
#define MENU_W     232
#define MENU_ROW   26

/* ---- 窗口 = 一个 App 实例 ---- */
#define NWIN 12   /* 桌面同时最多开 12 个窗口（单实例 App 下即 12 个不同 App） */

struct dwin {
    struct gfx_rect r;
    int z;
    int app_id;       /* 指向 G_apps[] 的应用定义 */
    int visible;
    int minimized;    /* 最小化：visible 仍=1（任务栏保留），绘制/命中/聚焦均跳过 */
    int data;         /* 每实例小状态（滚动偏移等） */
    int seq;          /* 同 App 多实例序号（单实例下恒 0） */
};

static struct dwin G_win[NWIN];
static int G_focus = -1;
static int G_inited;
static u64 G_last_sec;
static int G_open_seq;

/* ---- 鼠标状态 ---- */
static int G_mx, G_my, G_mprev;
static int G_drag = -1;                 /* 正在拖动的窗口索引 */
static int G_drag_dx, G_drag_dy;
static int G_menu_on;
static int G_cal_on;                    /* 鼠标校准覆盖层是否显示 */
static int G_cal_done;                  /* 已算出偏移，等待用户点一下返回 */
static char G_cal_msg[64];              /* 校准结果（ASCII，字库必定有） */

/* ---- App 框架 ---- */
struct gapp {
    const char *name;          /* 开始菜单 / 任务栏显示名 */
    u32 accent;
    int  single;               /* 1 = 单实例（已开则聚焦，不新建） */
    void (*init)(int win);     /* 开窗时调用一次（可 NULL） */
    void (*draw)(struct gfx_rect cr, int focused, int win);
    void (*press)(struct gfx_rect cr, int lx, int ly, int win);  /* 内容区局部坐标 */
    void (*key)(char c, int win);
    void (*scroll)(int dy, int win);
};
/* App 编号（同时是 G_apps 下标） */
enum {
    A_TERM = 0, A_INFO, A_DEMO, A_HELP,
    A_CALC, A_MON, A_SET, A_ABOUT, A_FILES, A_EDIT, NAPP
};
static const struct gapp *G_apps[NAPP];
static char G_launch_path[PATH_CAP]; /* open_app 前由调用方填；统一 160，杜绝与 full/tmp 错配 */

/* ---- 开始菜单项（动态生成） ---- */
#define MAX_MENU 24
struct menuitem { const char *label; int type; int app_id; const char *cmd; };
#define MI_APP  0
#define MI_EXEC 1
static struct menuitem G_menu[MAX_MENU];
static int G_menu_n;

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
static int my_strlen(const char *s){ int n=0; while(*s) n++; return n; }
static void my_strcpy(char *d, const char *s){ while(*s){*d++=*s++;} *d=0; }
static int my_atoi(const char *s){ int v=0,neg=0; while(*s==' '||*s=='-'){ if(*s=='-')neg=1; s++; } while(*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; } return neg?-v:v; }
static int my_itoa(char *b, long v)
{
    int n=0; char t[16]; int m=0;
    if (v < 0) { b[n++]='-'; v=-v; }
    if (v == 0) t[m++]='0';
    while (v) { t[m++] = (char)('0' + v%10); v/=10; }
    while (m) b[n++] = t[--m];
    b[n]=0; return n;
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
/* 带符号十进制（校准偏移可能为负），返回写入长度 */
static int fmt_signed(char *p, int v)
{
    int n = 0;
    if (v < 0) { p[n++] = '-'; v = -v; }
    else p[n++] = '+';
    char t[12]; int m = 0;
    if (!v) t[m++] = '0';
    while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m) p[n++] = t[--m];
    return n;
}

/* 前向声明（函数定义顺序在下方） */
static int my_strcmp(const char *a, const char *b);
static int my_strcmp_eq(const char *a, const char *b);
static void raise_focus_shim(void);
static void refocus(void);
static void fm_draw(struct gfx_rect cr, int f, int w);

/* 内容区内画一个按钮（base=内容矩形；x/y/w/h 为内容内局部坐标） */
static void app_button(struct gfx_rect base, int x, int y, int w, int h,
                       const char *label, u32 bg)
{
    struct gfx_rect br = {base.x + x, base.y + y, w, h};
    gfx_fill(br, bg);
    gfx_rect_draw(br, GFX_RGB(90, 120, 160), 1);
    int tw = gfx_text_w(label);
    gfx_text(br.x + (br.w - tw)/2, br.y + (br.h - 16)/2, label, C_WHITE, C_TRANSP);
}
/* 判定内容区局部坐标 (lx,ly) 是否落在 (x,y,w,h) 内 */
static int app_hit(int lx, int ly, int x, int y, int w, int h)
{
    return lx >= x && lx < x + w && ly >= y && ly < y + h;
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
static struct gfx_rect cursor_rect(int x, int y)
{
    return (struct gfx_rect){x, y, CURSOR_W + 2, CURSOR_H + 2};
}
static struct gfx_rect close_rect(struct gfx_rect r)
{
    return (struct gfx_rect){r.x + r.w - 24, r.y + 4, 20, 22};
}
static struct gfx_rect min_rect(struct gfx_rect r)
{
    return (struct gfx_rect){r.x + r.w - 48, r.y + 4, 20, 22};
}

/* ---- 开始菜单：动态列出全部 App + 关机/重启 ---- */
static void menu_build(void)
{
    G_menu_n = 0;
    for (int i = 0; i < NAPP && G_menu_n < MAX_MENU; i++) {
        if (!G_apps[i] || !G_apps[i]->name) continue;
        G_menu[G_menu_n].label  = G_apps[i]->name;
        G_menu[G_menu_n].type   = MI_APP;
        G_menu[G_menu_n].app_id = i;
        G_menu[G_menu_n].cmd    = 0;
        G_menu_n++;
    }
    if (G_menu_n < MAX_MENU) {
        G_menu[G_menu_n].label = "关机"; G_menu[G_menu_n].type = MI_EXEC;
        G_menu[G_menu_n].cmd = "poweroff"; G_menu_n++;
    }
    if (G_menu_n < MAX_MENU) {
        G_menu[G_menu_n].label = "重启"; G_menu[G_menu_n].type = MI_EXEC;
        G_menu[G_menu_n].cmd = "reboot"; G_menu_n++;
    }
}
static struct gfx_rect menu_rect(void)
{
    menu_build();
    int h = 8 + G_menu_n * MENU_ROW + 8;
    return (struct gfx_rect){10, gfx_height() - TASKBAR_H - h, MENU_W, h};
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

/* 打开一个 App（单实例已开则聚焦）；返回窗口槽位 */
static int open_app(int app_id)
{
    if (app_id < 0 || app_id >= NAPP || !G_apps[app_id]) return -1;
    if (G_apps[app_id]->single) {
        for (int i = 0; i < NWIN; i++)
        if (G_win[i].visible && G_win[i].app_id == app_id) {
            G_win[i].minimized = 0;
            G_focus = i; raise_focus_shim(); return i;
        }
    }
    int slot = -1;
    for (int i = 0; i < NWIN; i++)
        if (!G_win[i].visible) { slot = i; break; }
    if (slot < 0) {            /* 全开满：复用 z 最小的可见窗口 */
        slot = 0;
        for (int i = 1; i < NWIN; i++)
            if (G_win[i].z < G_win[slot].z) slot = i;
    }
    int big = (app_id == A_TERM);
    struct gfx_rect r = {
        40 + (G_open_seq % 6) * 26, TOPBAR_H + 6 + (G_open_seq % 5) * 24,
        big ? 460 : 380, big ? 340 : 300 };
    clamp_win(&r);
    G_win[slot].r = r;
    G_win[slot].app_id = app_id;
    G_win[slot].visible = 1;
    G_win[slot].minimized = 0;
    G_win[slot].data = 0;
    G_win[slot].seq = G_open_seq++;
    G_focus = slot;
    raise_focus_shim();
    if (G_apps[app_id]->init) G_apps[app_id]->init(slot);
    return slot;
}

/* z 序提升（gfx 纯函数版，避免重复依赖） */
static int top_z(void)
{
    int m = 0;
    for (int i = 0; i < NWIN; i++) if (G_win[i].z > m) m = G_win[i].z;
    return m + 1;
}
static void raise_focus_shim(void)
{
    if (G_focus < 0) return;
    G_win[G_focus].z = top_z();
    gfx_damage(full_screen());
}
static void refocus(void)
{
    for (int i = 0; i < NWIN; i++)
        if (G_win[i].visible && !G_win[i].minimized) { G_focus = i; return; }
    G_focus = -1;
}
static void focus_win(int i)
{
    if (i < 0 || i >= NWIN || !G_win[i].visible) return;
    G_focus = i;
    raise_focus_shim();
}
static void move_focus(int dx, int dy)
{
    if (G_focus < 0) return;
    G_win[G_focus].r.x += dx;
    G_win[G_focus].r.y += dy;
    clamp_win(&G_win[G_focus].r);
    gfx_damage(full_screen());      /* 整屏重画，杜绝移动残影 */
}
static int top_win_at(int x, int y)
{
    int best = -1, best_z = 0;
    for (int i = 0; i < NWIN; i++) {
        if (!G_win[i].visible || G_win[i].minimized) continue;
        if (!gfx_hit(G_win[i].r, x, y)) continue;
        if (best < 0 || G_win[i].z > best_z) { best = i; best_z = G_win[i].z; }
    }
    return best;
}

/* =================== App 实现 =================== */

/* —— 终端（复用 gfxterm） —— */
static void term_draw(struct gfx_rect cr, int f, int w){ (void)f;(void)w; gfxterm_draw(cr, f); }
static void term_key(char c, int w){ (void)w; gfxterm_key(c); }
static void term_scroll(int dy, int w){ (void)w; gfxterm_scroll(dy > 0 ? 2 : -2); }

/* —— 系统信息 / 图形演示 / 操作说明（静态文本） —— */
static void info_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    const char *lines[] = {
        "内核  LoongOS AArch64（自研 EL1）",
        "屏幕  32bpp 图形界面 800x600",
        "鼠标  点击 / 拖动窗口 / 校准",
        "终端  可执行全部 shell 命令",
        "电源  PSCI 优雅关机 / 重启",
        "App   开始菜单全列，点开即用",
    };
    for (int i = 0; i < 6; i++)
        gfx_text(cr.x + 16, cr.y + 12 + i * 22, lines[i], C_WIN_TEXT, C_TRANSP);
}
static void demo_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    const char *lines[] = {
        "图形栈特性演示：",
        "裁剪 / 命中 / z 序",
        "脏矩形局部重绘",
        "本窗口按 W A S D 移动",
        "本窗口按 1..9 聚焦",
        "Ctrl+Shift+F1 返回终端",
    };
    for (int i = 0; i < 6; i++)
        gfx_text(cr.x + 16, cr.y + 12 + i * 22, lines[i], C_WIN_TEXT, C_TRANSP);
}
static void help_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    const char *lines[] = {
        "鼠标左键  点击 / 拖动窗口",
        "开始菜单  全部功能入口",
        "终端窗口  直接敲命令",
        "终端里 W/A/S/D/数字 正常打字",
        "信息类窗口 WASD 移动 / Tab 切换",
        "Ctrl+Shift+F1 返回终端",
    };
    for (int i = 0; i < 6; i++)
        gfx_text(cr.x + 16, cr.y + 12 + i * 22, lines[i], C_WIN_TEXT, C_TRANSP);
}

/* —— 计算器 —— */
static char  calc_s[24] = "0";
static long  calc_acc;
static int   calc_op;       /* 0 无 / 1 + / 2 - / 3 * / 4 / */
static int   calc_fresh;    /* 1 = 当前显示是新数（下一数字键起新数） */
static int   calc_err;

static long calc_apply(long a, long b, int op)
{
    if (op == 1) return a + b;
    if (op == 2) return a - b;
    if (op == 3) return a * b;
    if (op == 4) { if (b == 0) { calc_err = 1; return 0; } return a / b; }
    return b;
}
static void calc_reset(void)
{
    calc_s[0]='0'; calc_s[1]=0; calc_acc=0; calc_op=0; calc_fresh=1; calc_err=0;
}
static void calc_digit(char k)
{
    if (calc_fresh) { calc_s[0]=k; calc_s[1]=0; calc_fresh=0; }
    else if (my_strlen(calc_s) < 15) { int n=my_strlen(calc_s); calc_s[n]=k; calc_s[n+1]=0; }
}
static void calc_op_do(int op)
{
    long v = my_atoi(calc_s);
    if (calc_op == 0) calc_acc = v;
    else calc_acc = calc_apply(calc_acc, v, calc_op);
    calc_op = op;
    if (calc_err) { calc_reset(); return; }
    my_itoa(calc_s, calc_acc); calc_fresh = 1;
}
static void calc_eq(void)
{
    if (calc_op != 0) {
        long v = my_atoi(calc_s);
        long r = calc_apply(calc_acc, v, calc_op);
        calc_acc = 0; calc_op = 0;
        if (calc_err) { calc_reset(); return; }
        my_itoa(calc_s, r);
    }
    calc_fresh = 1;
}
static void calc_key(char k, int w)
{
    (void)w;
    if (k >= '0' && k <= '9') calc_digit(k);
    else if (k == '+') calc_op_do(1);
    else if (k == '-') calc_op_do(2);
    else if (k == '*') calc_op_do(3);
    else if (k == '/') calc_op_do(4);
    else if (k == '=') calc_eq();
    else if (k == 'C' || k == 'c') calc_reset();
}
static const char calc_keys[4][5] = { "789/", "456*", "123-", "C0=+" };
static struct gfx_rect calc_key_rect(struct gfx_rect cr, int col, int row)
{
    int pad = 8, gap = 6, disp_h = 44;
    int gw = (cr.w - 2*pad - 3*gap) / 4;
    int gh = (cr.h - disp_h - pad*2 - 4*gap) / 4;
    return (struct gfx_rect){cr.x + pad + col*(gw+gap),
                             cr.y + disp_h + pad + row*(gh+gap), gw, gh};
}
static void calc_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    gfx_clip_push(cr);
    gfx_fill((struct gfx_rect){cr.x + 8, cr.y + 8, cr.w - 16, 40},
             GFX_RGB(228, 234, 244));
    gfx_rect_draw((struct gfx_rect){cr.x + 8, cr.y + 8, cr.w - 16, 40},
                  GFX_RGB(150, 170, 200), 1);
    const char *disp = calc_err ? "Err" : calc_s;
    int tw = gfx_text_w(disp);
    gfx_text(cr.x + cr.w - 12 - tw, cr.y + 8 + 12, disp, C_WIN_TEXT, C_TRANSP);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            struct gfx_rect br = calc_key_rect(cr, c, r);
            char k = calc_keys[r][c];
            u32 bg = (k=='+'||k=='-'||k=='*'||k=='/') ? C_TEAL
                   : (k=='='||k=='C') ? C_ORANGE : C_BTN;
            gfx_fill(br, bg);
            gfx_rect_draw(br, GFX_RGB(90,120,160), 1);
            int tw2 = gfx_text_w(&k);
            gfx_text(br.x + (br.w - tw2)/2, br.y + (br.h - 16)/2, &k, C_WHITE, C_TRANSP);
        }
    gfx_clip_pop();
}
static void calc_press(struct gfx_rect cr, int lx, int ly, int w)
{
    (void)w;
    if (ly < 52) return;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            struct gfx_rect br = calc_key_rect(cr, c, r);
            if (gfx_hit(br, lx + cr.x, ly + cr.y)) {
                calc_key(calc_keys[r][c], w);
                gfx_damage(full_screen());
                return;
            }
        }
}
static void calc_init(int w){ (void)w; calc_reset(); }

/* —— 系统监视器 —— */
static void mon_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    u32 used=0, total=0; fs_statfs(&used, &total);
    int ncpu = 0; for (int i = 0; i < 4; i++) if (smp_online[i]) ncpu++;
    char buf[48];
    gfx_text(cr.x + 12, cr.y + 10, "系统监视器（实时）", C_WIN_TEXT, C_TRANSP);
    int row = 0;
    #define MON(label, val) do { \
        int n = 0; const char *p = label; while (*p) buf[n++] = *p++; \
        buf[n++] = ' '; n += my_itoa(buf + n, (long)(val)); \
        gfx_text(cr.x + 12, cr.y + 36 + row*20, buf, C_WIN_TEXT, C_TRANSP); row++; \
    } while (0)
    MON("运行时长(s)", (long)(tick / 10));
    MON("唤醒频率(Hz)", (long)g_wake_hz);
    MON("Timer IRQ",   (long)g_timer_irqs);
    MON("在线核",      (long)ncpu);
    MON("桌面态",      (long)g_desktop_active);
    MON("磁盘块已用",  (long)used);
    MON("磁盘块总计",  (long)total);
    #undef MON
}

/* —— 设置 —— */
static void set_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    gfx_clip_push(cr);
    gfx_text(cr.x + 14, cr.y + 12, "设置", C_WIN_TEXT, C_TRANSP);
    gfx_text(cr.x + 14, cr.y + 36, "鼠标偏移不对？点下面重新校准", C_WIN_DIM, C_TRANSP);
    app_button(cr, 14, 60, 162, 26, "鼠标校准", C_BTN);
    app_button(cr, 14, 94, 162, 26, "关于本机", C_BTN);
    app_button(cr, 14, 128, 162, 26, "系统监视器", C_BTN);
    app_button(cr, 14, 162, 162, 26, "文件管理器", C_BTN);
    gfx_clip_pop();
}
static void set_press(struct gfx_rect cr, int lx, int ly, int w)
{
    (void)w;
    if (app_hit(lx, ly, 14, 60, 162, 26)) {
        G_cal_on = 1; G_cal_done = 0; G_cal_msg[0] = 0; G_menu_on = 0;
        gfx_damage(full_screen());
    } else if (app_hit(lx, ly, 14, 94, 162, 26)) {
        open_app(A_ABOUT);
    } else if (app_hit(lx, ly, 14, 128, 162, 26)) {
        open_app(A_MON);
    } else if (app_hit(lx, ly, 14, 162, 162, 26)) {
        open_app(A_FILES);
    }
}

/* —— 关于 —— */
static void about_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    const char *lines[] = {
        "灵珑系统 · LoongOS",
        "自研 64 位 ARM(AArch64) 操作系统",
        "从 boot.S 第一行手写内核：",
        "GIC / 调度 / FS / ELF / 多设备HID",
        "NVMe · virtio · TCP · 图形桌面",
        "GUI 应用生态（开始菜单启动器）",
    };
    for (int i = 0; i < 6; i++)
        gfx_text(cr.x + 16, cr.y + 14 + i * 22,
                 i==0 ? lines[0] : lines[i], i==0 ? C_BLUE : C_WIN_TEXT, C_TRANSP);
}

/* —— 文件管理器 —— */
static char fm_path[PATH_CAP] = "/";
static int  fm_scroll;
static int  fm_overlong;   /* 上一次进目录因路径过长被拒 → 绘制时给用户可见提示 */
static int my_strcmp(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }
static int my_strcmp_eq(const char *a, const char *b){ return my_strcmp(a,b)==0; }
/* 第三十六轮修 H1：统一走 path.h 的带 cap 拼接，返回 -1 表示会越界（拒绝，不写） */
static int fm_full(char *out, int cap, const char *child)
{
    return path_join(out, cap, fm_path, child);
}

/* 枚举当前目录，对第 idx 项回调；返回找到的项信息 */
struct fm_find { int idx; int got; int ino; int isdir; char name[64]; };
static void fm_find_cb(const struct dirent *d, int ino, void *arg)
{
    struct fm_find *f = arg;
    if (!d || d->inum == 0) return;
    if (d->name[0] == 0) return;
    int cur = f->idx++;
    if (cur == f->got) {
        f->ino = ino; f->isdir = fs_isdir(ino);
        int i=0; while (d->name[i] && i<63) { f->name[i]=d->name[i]; i++; } f->name[i]=0;
    }
}
struct fm_draw_ctx { struct gfx_rect cr; int y0; int rowh; int scroll; int has_up; int cnt; };
static void fm_draw_cb(const struct dirent *d, int ino, void *arg)
{
    struct fm_draw_ctx *c = arg;
    if (!d || d->inum == 0 || d->name[0]==0) return;
    int ri = c->cnt + (c->has_up ? 1 : 0);
    c->cnt++;
    if (ri < c->scroll) return;
    int yy = c->y0 + (ri - c->scroll) * c->rowh;
    if (yy > c->cr.y + c->cr.h - 16) return;
    int isdir = fs_isdir(ino);
    char nm[72]; int n=0; nm[n++]=' ';
    if (isdir) nm[n++]='[';
    int i=0; while (d->name[i] && n<69) nm[n++]=d->name[i++];  /* 留 2 字节给 ']' 和 NUL */
    if (isdir && n < 71) nm[n++]=']';
    nm[n]=0;
    gfx_text(c->cr.x + 16, yy, nm, isdir ? C_BLUE : C_WIN_TEXT, C_TRANSP);
}
static void fm_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    gfx_clip_push(cr);
    if (fm_overlong)
        gfx_text(cr.x + 10, cr.y + 6, "路径过长，已拒绝进入（上限 160 字节）", C_RED, C_TRANSP);
    else
        gfx_text(cr.x + 10, cr.y + 6, fm_path, C_WIN_TEXT, C_TRANSP);
    gfx_rect_draw((struct gfx_rect){cr.x+8, cr.y+24, cr.w-16, cr.h-32}, GFX_RGB(200,210,224),1);
    int y0 = cr.y + 30, rowh = 20;
    int ino = fs_lookup(fm_path);
    int has_up = my_strcmp_eq(fm_path, "/") ? 0 : 1;
    if (has_up)
        gfx_text(cr.x + 16, y0, ".. (上级)", C_VIOLET, C_TRANSP);
    if (ino > 0) {
        struct fm_draw_ctx ctx;
        ctx.cr = cr; ctx.y0 = y0; ctx.rowh = rowh;
        ctx.scroll = fm_scroll; ctx.has_up = has_up; ctx.cnt = 0;
        fs_list(ino, fm_draw_cb, &ctx);
    }
    gfx_clip_pop();
}
static void fm_press(struct gfx_rect cr, int lx, int ly, int w)
{
    (void)w;
    if (lx < 8 || lx > cr.w - 8 || ly < 30) return;
    int row = fm_scroll + (ly - 30) / 20;
    int ino = fs_lookup(fm_path);
    int has_up = my_strcmp_eq(fm_path, "/") ? 0 : 1;
    if (has_up && row == 0) {                 /* 上级目录 */
        char tmp[PATH_CAP];
        if (path_parent(tmp, sizeof tmp, fm_path) > 0) {
            my_strcpy(fm_path, tmp); fm_scroll = 0; fm_overlong = 0;
        }
        gfx_damage(full_screen());
        return;
    }
    int target = has_up ? row - 1 : row;
    if (target < 0 || ino <= 0) return;
    struct fm_find f = {.idx=0, .got=target};
    fs_list(ino, fm_find_cb, &f);
    if (f.ino <= 0) return;
    char full[PATH_CAP];
    if (fm_full(full, sizeof full, f.name) < 0) {   /* H1：超长拒绝，绝不越界写 */
        fm_overlong = 1; gfx_damage(full_screen()); return;
    }
    fm_overlong = 0;
    if (f.isdir) {
        my_strcpy(fm_path, full); fm_scroll = 0;   /* full 与 fm_path 同为 PATH_CAP */
    } else {
        my_strcpy(G_launch_path, full);            /* 两者同为 PATH_CAP */
        open_app(A_EDIT);
    }
    gfx_damage(full_screen());
}
static void fm_scroll_h(int dy, int w){ (void)w; fm_scroll += (dy>0?-3:3); if(fm_scroll<0) fm_scroll=0; gfx_damage(full_screen()); }

/* —— 文本编辑器 / 查看器 —— */
static char ed_path[PATH_CAP] = "/未命名.txt";  /* 与 G_launch_path 同为 PATH_CAP，ed_load 拷贝安全 */
static char ed_buf[1500];
static int  ed_len;
static int  ed_cur;
static int  ed_scroll;
static int  ed_dirty;
static int  ed_msg_t;     /* >0 时显示「已保存」 */
static void ed_load(void)
{
    ed_len = 0; ed_buf[0] = 0; ed_cur = 0; ed_dirty = 0; ed_msg_t = 0;
    if (G_launch_path[0]) my_strcpy(ed_path, G_launch_path);
    else my_strcpy(ed_path, "/未命名.txt");
    G_launch_path[0] = 0;
    int ino = fs_lookup(ed_path);
    if (ino > 0 && fs_filetype(ino) == FS_T_FILE) {
        int n = fs_readfile(ino, (u8*)ed_buf, sizeof(ed_buf) - 1);
        if (n > 0) { ed_len = n; ed_buf[n] = 0; ed_cur = n; }
    }
}
static void ed_save(void)
{
    int ino = fs_lookup(ed_path);
    if (ino <= 0) fs_createfile(ed_path);
    ino = fs_lookup(ed_path);
    if (ino > 0) { fs_setfile(ino, (u8*)ed_buf, (u32)ed_len); ed_dirty = 0; ed_msg_t = 1; }
}
static void ed_draw(struct gfx_rect cr, int f, int w)
{
    (void)f;(void)w;
    gfx_clip_push(cr);
    gfx_text(cr.x + 8, cr.y + 6, ed_path, C_WIN_TEXT, C_TRANSP);
    if (ed_dirty) gfx_text(cr.x + cr.w - 20, cr.y + 6, "*", C_RED, C_TRANSP);
    if (ed_msg_t) gfx_text(cr.x + cr.w - 70, cr.y + 6, "已保存", C_TEAL, C_TRANSP);
    app_button(cr, cr.w - 78, 2, 70, 22, "保存", C_BTN);
    int x0 = cr.x + 8, y0 = cr.y + 32, maxw = cr.w - 16;
    int cpl = maxw / 8;
    int row = 0, start = 0, curx = -1, cury = -1;
    for (int i = 0; i <= ed_len; i++) {
        if (i == ed_len || ed_buf[i] == '\n') {
            if (row >= ed_scroll) {
                int yy = y0 + (row - ed_scroll) * 18;
                if (yy > cr.y + cr.h - 18) break;
                int n = i - start; if (n > cpl) n = cpl;
                /* 计算光标位置（若在本行） */
                if (ed_cur >= start && ed_cur <= i) {
                    curx = x0 + (ed_cur - start) * 8;
                    cury = yy;
                }
                for (int k = 0; k < n; k++)
                    gfx_text(x0 + k*8, yy, &ed_buf[start + k], C_WIN_TEXT, C_TRANSP);
            }
            start = i + 1; row++;
        }
    }
    if (curx >= 0 && cury >= 0)
        gfx_fill((struct gfx_rect){curx, cury, 8, 16}, C_BLUE);
    gfx_clip_pop();
}
static void ed_press(struct gfx_rect cr, int lx, int ly, int w)
{
    (void)w;
    if (app_hit(lx, ly, cr.w - 78, 2, 70, 22)) { ed_save(); gfx_damage(full_screen()); }
}
static void ed_key(char c, int w)
{
    (void)w;
    if (c == '\b' || c == 0x7f) {                 /* 退格 */
        if (ed_cur > 0) {
            for (int i = ed_cur; i < ed_len; i++) ed_buf[i-1] = ed_buf[i];
            ed_len--; ed_cur--; ed_dirty = 1; ed_msg_t = 0;
        }
    } else if (c == '\n' || c == '\r') {
        if (ed_len < (int)sizeof(ed_buf) - 1) {
            for (int i = ed_len; i > ed_cur; i--) ed_buf[i] = ed_buf[i-1];
            ed_buf[ed_cur++] = '\n'; ed_len++; ed_dirty = 1; ed_msg_t = 0;
        }
    } else if (c >= 0x20 && c < 0x7f) {          /* 可打印 ASCII */
        if (ed_len < (int)sizeof(ed_buf) - 1) {
            for (int i = ed_len; i > ed_cur; i--) ed_buf[i] = ed_buf[i-1];
            ed_buf[ed_cur++] = c; ed_len++; ed_dirty = 1; ed_msg_t = 0;
        }
    }
    gfx_damage(full_screen());
}
static void ed_scroll_h(int dy, int w){ (void)w; ed_scroll += (dy>0?-3:3); if(ed_scroll<0)ed_scroll=0; gfx_damage(full_screen()); }
static void ed_init(int w){ (void)w; ed_load(); }

/* =================== App 注册表 =================== */
static const struct gapp G_app_defs[NAPP] = {
    [A_TERM]    = {"终端",       C_BLUE,   1, 0, term_draw,   0,          term_key, term_scroll},
    [A_INFO]    = {"系统信息",   C_TEAL,   1, 0, info_draw,   0, 0, 0},
    [A_DEMO]    = {"图形演示",   C_VIOLET, 1, 0, demo_draw,   0, 0, 0},
    [A_HELP]    = {"操作说明",   C_TEAL,   1, 0, help_draw,   0, 0, 0},
    [A_CALC]    = {"计算器",     C_BLUE,   1, calc_init, calc_draw, calc_press, calc_key, 0},
    [A_MON]     = {"系统监视器", C_TEAL,   1, 0, mon_draw,    0, 0, 0},
    [A_SET]     = {"设置",       C_VIOLET, 1, 0, set_draw,    set_press, 0, 0},
    [A_ABOUT]   = {"关于",       C_BLUE,   1, 0, about_draw,  0, 0, 0},
    [A_FILES]   = {"文件管理器", C_ORANGE, 1, 0, fm_draw,     fm_press, 0, fm_scroll_h},
    [A_EDIT]    = {"文本编辑器", C_BLUE,   1, ed_init, ed_draw, ed_press, ed_key, ed_scroll_h},
};
static void apps_register(void)
{
    for (int i = 0; i < NAPP; i++) G_apps[i] = &G_app_defs[i];
}

/* ---- 各层绘制 ---- */
static void draw_wallpaper(void)
{
    int w = gfx_width(), h = gfx_height();
    for (int y = 0; y < h; y++)
        gfx_fill((struct gfx_rect){0, y, w, 1},
                 mix(C_WALL_A, C_WALL_B, h > 1 ? (y * 255) / (h - 1) : 0));
    for (int i = 0; i < 7; i++)
        gfx_line(w - 260 + i * 22, h - TASKBAR_H, w - 90 + i * 22, TOPBAR_H + 56, C_DECO);
}
static void draw_topbar(void)
{
    int w = gfx_width();
    gfx_fill((struct gfx_rect){0, 0, w, TOPBAR_H}, C_TOPBAR);
    gfx_fill((struct gfx_rect){0, TOPBAR_H - 2, w, 2}, C_BLUE);
    gfx_text(16, 11, "灵珑系统  LoongOS", C_WHITE, C_TRANSP);
    const char *hint = "图形桌面 P4 · GUI 应用生态   |   Ctrl+Shift+F1 返回终端";
    int hw = gfx_text_w(hint);
    if (w - hw - 16 > 260)
        gfx_text(w - hw - 16, 11, hint, C_ACCENT, C_TRANSP);
}

static void draw_button_rect(struct gfx_rect r, const char *label, u32 bg, u32 fg)
{
    gfx_fill(r, bg);
    gfx_rect_draw(r, GFX_RGB(90, 120, 160), 1);
    int tw = gfx_text_w(label);
    gfx_text(r.x + (r.w - tw) / 2, r.y + (r.h - 16) / 2, label, fg, C_TRANSP);
}

static void draw_one_win(int k, int focused)
{
    struct dwin *w = &G_win[k];
    struct gfx_rect r = w->r;
    if (w->minimized) return;          /* 最小化窗口不绘制 */
    gfx_fill((struct gfx_rect){r.x + 4, r.y + 4, r.w, r.h}, C_SHADOW);
    gfx_fill(r, C_WIN_BG);
    gfx_rect_draw(r, C_BORDER, 1);
    gfx_fill((struct gfx_rect){r.x, r.y, r.w, TITLE_H},
             focused ? G_apps[w->app_id]->accent : C_TITLE_DIM);
    struct gfx_rect cr = close_rect(r);
    gfx_fill((struct gfx_rect){cr.x + 3, cr.y + 6, 14, 12}, focused ? C_WHITE : C_WIN_BG);
    gfx_text(cr.x + 5, cr.y + 4, "X", focused ? G_apps[w->app_id]->accent : C_TITLE_DIM, C_TRANSP);
    struct gfx_rect mr = min_rect(r);
    gfx_fill((struct gfx_rect){mr.x + 3, mr.y + 14, 14, 4}, focused ? C_WHITE : C_WIN_BG);
    gfx_text(r.x + 12, r.y + 6, G_apps[w->app_id]->name, C_WHITE, C_TRANSP);

    struct gfx_rect content = (struct gfx_rect){r.x + 1, r.y + TITLE_H, r.w - 2, r.h - TITLE_H - 1};
    gfx_clip_push(content);
    gfx_fill((struct gfx_rect){r.x + 1, r.y + TITLE_H, r.w - 2, 2}, G_apps[w->app_id]->accent);
    if (G_apps[w->app_id]->draw) G_apps[w->app_id]->draw(content, focused, k);
    gfx_clip_pop();
}

static void draw_windows(void)
{
    int order[NWIN];
    for (int i = 0; i < NWIN; i++) order[i] = i;
    for (int i = 0; i < NWIN; i++)
        for (int j = i + 1; j < NWIN; j++)
            if (G_win[order[j]].z < G_win[order[i]].z) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
    for (int i = 0; i < NWIN; i++) {
        int k = order[i];
        if (G_win[k].visible) draw_one_win(k, k == G_focus);
    }
}

static int visible_count(void)
{
    int n = 0; for (int i = 0; i < NWIN; i++) if (G_win[i].visible) n++;
    return n;
}
/* 任务栏按钮：均分宽度，永远不溢出 */
static struct gfx_rect taskbtn_rect_dyn(int vis_idx)
{
    int n = visible_count();
    int W = gfx_width();
    int bw = n > 0 ? (W - 100) / n : 90;
    if (bw > 96) bw = 96;
    if (bw < 56) bw = 56;
    return (struct gfx_rect){96 + vis_idx * bw, gfx_height() - TASKBAR_H + 8, bw - 6, 26};
}
static void draw_taskbar(void)
{
    int w = gfx_width(), h = gfx_height();
    int ty = h - TASKBAR_H;
    gfx_fill((struct gfx_rect){0, ty, w, TASKBAR_H}, C_TASKBAR);
    gfx_fill((struct gfx_rect){0, ty, w, 1}, C_TB_LINE);
    struct gfx_rect sr = start_rect();
    draw_button_rect(sr, "开始", G_menu_on ? C_TEAL : C_BLUE, C_WHITE);
    int vi = 0;
    for (int i = 0; i < NWIN; i++) {
        if (!G_win[i].visible) continue;
        struct gfx_rect br = taskbtn_rect_dyn(vi++);
        u32 bg = (i == G_focus) ? C_BTN_HI
               : (G_win[i].minimized ? GFX_RGB(20, 32, 52) : GFX_RGB(28, 44, 70));
        draw_button_rect(br, G_apps[G_win[i].app_id]->name, bg, C_WHITE);
    }
    char tb[32];
    fmt_time(tb, tick / 10);
    gfx_text(w - gfx_text_w(tb) - 16, ty + 13, tb, C_ACCENT, C_TRANSP);
}

static void draw_menu(void)
{
    if (!G_menu_on) return;
    menu_build();
    struct gfx_rect m = menu_rect();
    gfx_fill((struct gfx_rect){m.x + 3, m.y + 3, m.w, m.h}, C_SHADOW);
    gfx_fill(m, C_MENU_BG);
    gfx_rect_draw(m, C_TB_LINE, 1);
    gfx_fill((struct gfx_rect){m.x, m.y, m.w, 2}, C_TEAL);
    gfx_text(m.x + 10, m.y + 8, "开始", C_ACCENT, C_TRANSP);
    for (int i = 0; i < G_menu_n; i++) {
        struct gfx_rect br = {m.x + 8, m.y + 8 + i * MENU_ROW, m.w - 16, 22};
        int hover = gfx_hit(br, G_mx, G_my);
        u32 bg = hover ? C_BTN_HI : C_BTN;
        draw_button_rect(br, G_menu[i].label, bg, C_WHITE);
    }
}

static void draw_cursor(void)
{
    for (int i = 0; i < CURSOR_H; i++) {
        int half = i / 2;
        gfx_fill((struct gfx_rect){G_mx, G_my + i, half + 2, 1}, GFX_RGB(8, 12, 20));
    }
    for (int i = 1; i < CURSOR_H - 2; i++) {
        int half = i / 2;
        gfx_fill((struct gfx_rect){G_mx + 1, G_my + i, half, 1}, C_WHITE);
    }
}

/* 鼠标校准覆盖层：靶心固定在屏幕正中，用户点它，内核用收到坐标减靶心得偏移并保存。 */
static void draw_calibrate(void)
{
    int w = gfx_width(), h = gfx_height();
    int cx = w / 2, cy = h / 2;
    gfx_fill((struct gfx_rect){0, 0, w, h}, C_TOPBAR);
    const char *t1 = "鼠标校准";
    gfx_text((w - gfx_text_w(t1)) / 2, 56, t1, C_WHITE, C_TRANSP);
    const char *t2 = "用鼠标点击正中红色靶心";
    gfx_text((w - gfx_text_w(t2)) / 2, 88, t2, C_ACCENT, C_TRANSP);
    const char *t3 = "光标与真实鼠标差多少 会自动补偿并保存";
    gfx_text((w - gfx_text_w(t3)) / 2, 110, t3, C_WIN_DIM, C_TRANSP);
    const char *t4 = G_cal_done ? "已保存 再点一下返回桌面"
                                : "点任意位置完成 | Ctrl Shift F1 放弃";
    gfx_text((w - gfx_text_w(t4)) / 2, 132, t4,
             G_cal_done ? C_ACCENT : C_WIN_DIM, C_TRANSP);
    gfx_fill((struct gfx_rect){cx - 1, cy - 70, 2, 140}, C_WHITE);
    gfx_fill((struct gfx_rect){cx - 70, cy - 1, 140, 2}, C_WHITE);
    gfx_fill((struct gfx_rect){cx - 16, cy - 16, 32, 32}, C_RED);
    gfx_fill((struct gfx_rect){cx - 10, cy - 10, 20, 20}, C_WHITE);
    gfx_fill((struct gfx_rect){cx - 4,  cy - 4,  8,  8},  C_RED);
    if (G_cal_msg[0])
        gfx_text((w - gfx_text_w(G_cal_msg)) / 2, h - 90, G_cal_msg, C_ACCENT, C_TRANSP);
    draw_cursor();
}

static void redraw(struct gfx_rect r)
{
    (void)r;
    if (G_cal_on) { draw_calibrate(); return; }
    draw_wallpaper();
    draw_topbar();
    draw_windows();
    draw_taskbar();
    draw_menu();
    draw_cursor();
}

/* ---- 鼠标事件 ---- */
static void handle_press(void)
{
    int x = G_mx, y = G_my;

    if (G_cal_on) {
        if (G_cal_done) { G_cal_on = 0; G_cal_done = 0; gfx_damage(full_screen()); return; }
        int cx = gfx_width() / 2, cy = gfx_height() / 2;
        int dx = x - cx, dy = y - cy;
        mouse_cal_set(dx, dy);
        char b[64]; int n = 0;
        const char *p = "mousecal: dx=";
        while (*p) b[n++] = *p++;
        n += fmt_signed(b + n, dx);
        p = " dy="; while (*p) b[n++] = *p++;
        n += fmt_signed(b + n, dy);
        p = " (saved)"; while (*p) b[n++] = *p++;
        b[n] = 0;
        for (int i = 0; i <= n && i < (int)sizeof G_cal_msg; i++) G_cal_msg[i] = b[i];
        G_cal_done = 1; G_mx = cx; G_my = cy;
        gfx_damage(full_screen());
        return;
    }

    /* 1. 任务栏：开始按钮 */
    if (gfx_hit(start_rect(), x, y)) {
        G_menu_on = !G_menu_on;
        gfx_damage(full_screen());
        return;
    }
    /* 2. 任务栏：已打开窗口按钮 */
    {
        int vi = 0;
        for (int i = 0; i < NWIN; i++) {
            if (!G_win[i].visible) continue;
            if (gfx_hit(taskbtn_rect_dyn(vi), x, y)) {
                if (G_win[i].minimized)      { G_win[i].minimized = 0; G_focus = i; raise_focus_shim(); }
                else if (i == G_focus)       { G_win[i].minimized = 1; refocus(); }
                else                          focus_win(i);
                G_menu_on = 0; gfx_damage(full_screen()); return;
            }
            vi++;
        }
    }
    /* 3. 开始菜单（打开时优先吃点击） */
    if (G_menu_on) {
        menu_build();
        struct gfx_rect m = menu_rect();
        for (int i = 0; i < G_menu_n; i++) {
            struct gfx_rect br = {m.x + 8, m.y + 8 + i * MENU_ROW, m.w - 16, 22};
            if (gfx_hit(br, x, y)) {
                G_menu_on = 0;
                if (G_menu[i].type == MI_APP) open_app(G_menu[i].app_id);
                else gfxterm_exec(G_menu[i].cmd);
                gfx_damage(full_screen());
                return;
            }
        }
        if (!gfx_hit(m, x, y)) { G_menu_on = 0; gfx_damage(full_screen()); }
        return;
    }
    /* 4. 最顶层命中的窗口 */
    int wi = top_win_at(x, y);
    if (wi < 0) return;
    struct gfx_rect r = G_win[wi].r;

    if (gfx_hit(min_rect(r), x, y) && y < r.y + TITLE_H) {
        G_win[wi].minimized = !G_win[wi].minimized;
        if (G_win[wi].minimized) { if (G_focus == wi) refocus(); }
        else { G_focus = wi; raise_focus_shim(); }
        gfx_damage(full_screen());
        return;
    }
    if (gfx_hit(close_rect(r), x, y) && y < r.y + TITLE_H) {
        G_win[wi].visible = 0;
        if (G_focus == wi) refocus();
        gfx_damage(full_screen());
        return;
    }
    focus_win(wi);
    if (y < r.y + TITLE_H) {
        G_drag = wi; G_drag_dx = x - r.x; G_drag_dy = y - r.y;
        return;
    }
    /* 内容区：交给 App 处理 */
    struct gfx_rect cr = {r.x + 1, r.y + TITLE_H, r.w - 2, r.h - TITLE_H - 1};
    int lx = x - cr.x, ly = y - cr.y;
    if (G_apps[G_win[wi].app_id]->press)
        G_apps[G_win[wi].app_id]->press(cr, lx, ly, wi);
}

static int poll_mouse(void)
{
    int nx, ny, wh;
    int changed = 0;
    if (mouse_get(&nx, &ny, 0, &wh)) {
        struct gfx_rect old = cursor_rect(G_mx, G_my);
        int moved = (nx != G_mx || ny != G_my);
        G_mx = nx; G_my = ny;
        gfx_damage(gfx_union(old, cursor_rect(G_mx, G_my)));
        changed = 1;
        if (wh) {
            int dy = wh > 0 ? -1 : 1;
            if (G_focus >= 0 && G_apps[G_win[G_focus].app_id]->scroll)
                G_apps[G_win[G_focus].app_id]->scroll(dy, G_focus);
            else gfxterm_scroll(wh > 0 ? 2 : -2);
            gfx_damage(G_win[G_focus >= 0 ? G_focus : 0].r);
        }
        if (G_drag >= 0 && moved) {
            struct gfx_rect *r = &G_win[G_drag].r;
            r->x = G_mx - G_drag_dx; r->y = G_my - G_drag_dy;
            clamp_win(r); gfx_damage(full_screen());
        }
    }
    int st;
    while (mouse_pop_btn(&st)) {
        int pressed  = (st & 1) && !(G_mprev & 1);
        int released = !(st & 1) && (G_mprev & 1);
        if (pressed)  handle_press();
        if (released) G_drag = -1;
        G_mprev = st;
        changed = 1;
    }
    return changed;
}

/* ---- 键盘 ---- */
static void cycle_focus(void)
{
    int n = G_focus;
    for (int k = 1; k <= NWIN; k++) {
        int i = (n + k) % NWIN;
        if (G_win[i].visible && !G_win[i].minimized) { focus_win(i); return; }
    }
}
static void focus_nth_visible(int nth)
{
    int seen = 0;
    for (int i = 0; i < NWIN; i++)
        if (G_win[i].visible && !G_win[i].minimized) {
            if (++seen == nth) { focus_win(i); return; }
        }
}
static void handle_key(char c)
{
    /* 聚焦"有键盘输入"的 App（终端 / 编辑器 / 计算器，注册了 key 回调）时，
       所有按键（含 W A S D、数字、Tab）一律原样交给它，绝不被桌面热键抢走。
       否则像终端里敲 whoami、ls /dev/sda1、Tab 补全 这类输入会被桌面吞掉。 */
    if (G_focus >= 0 && G_apps[G_win[G_focus].app_id]->key) {
        G_apps[G_win[G_focus].app_id]->key(c, G_focus);
        return;
    }
    /* 以下桌面级热键只在聚焦"无键盘输入"的窗口（信息/演示/说明/监视器等）时生效 */
    if (c == '\t') { cycle_focus(); return; }
    if (c >= '1' && c <= '9') { focus_nth_visible(c - '0'); return; }
    if (c == 'w' || c == 'W') { move_focus(0, -STEP); return; }
    if (c == 's' || c == 'S') { move_focus(0,  STEP); return; }
    if (c == 'a' || c == 'A') { move_focus(-STEP, 0); return; }
    if (c == 'd' || c == 'D') { move_focus( STEP, 0); return; }
}

/* ---- 桌面主循环 ---- */
void desktop_run(void)
{
    if (!gfx_ready()) {
        kputs("[desktop] 无帧缓冲，保持串口 shell\n");
        return;
    }
    gfx_take_screen();
    gfx_set_redraw(redraw);
    G_cal_on = 0; G_cal_done = 0;

    if (!G_inited) {
        apps_register();
        G_focus = -1;
        G_open_seq = 0;
        G_mx = gfx_width() / 2; G_my = gfx_height() / 2;
        G_mprev = 0;
        /* 默认打开几个窗口（与旧版布局一致） */
        open_app(A_TERM);
        open_app(A_INFO);
        open_app(A_DEMO);
        open_app(A_HELP);
        G_inited = 1;
    }
    gfxterm_init();
    fb_console_reset();
    gfx_damage(full_screen());
    gfx_present();
    kputs("[desktop] 图形桌面已就绪：点「开始」打开应用，终端可跑所有命令\n");
    kputs("[desktop] 退出请按 Ctrl+Shift+F1（串口输入会自动让位）\n");

    u64 freq = dsk_cntfrq();
    if (!freq) freq = 24000000ull;
    u64 frame_cyc = freq / 60; if (!frame_cyc) frame_cyc = 1;
    u64 ms_cyc = freq / 1000; if (!ms_cyc) ms_cyc = 1;
    u64 next_frame = dsk_cntpct() + frame_cyc;
    u64 last_tick = tick;

    #define WAKE_ACTIVE_HZ 1000
    #define WAKE_IDLE_HZ    250
    #define WAKE_DEEP_HZ    120
    g_desktop_active = 1;
    timer_set_wake_hz(WAKE_ACTIVE_HZ);
    u32 cur_wake = WAKE_ACTIVE_HZ;
    u64 last_input_tick = tick;

    for (;;) {
        usb_kbd_poll();
        if (desktop_ring_peek_uart()) break;
        if (desktop_exit_requested()) break;

        int input = 0, real_input = 0;
        char c;
        while (desktop_ring_pop_kbd(&c)) {
            handle_key(c);
            input = 1; real_input = 1;
        }
        if (poll_mouse()) { input = 1; real_input = 1; }

        if (tick != last_tick) {
            last_tick = tick;
            smp_tick_inc();
            net_poll();
            u64 sec = tick / 10;
            if (sec != G_last_sec) { G_last_sec = sec; gfx_damage(clock_rect()); input = 1; }
            if (ed_msg_t) { ed_msg_t = 0; gfx_damage(full_screen()); }
        }

        u64 now = dsk_cntpct();
        int frame_due = (now >= next_frame);
        if (frame_due) { next_frame = now + frame_cyc; gfxterm_blink(now / ms_cyc); }

        int d = gfxterm_dirty();
        if (d == 2) { gfx_damage(G_win[G_focus >= 0 ? G_focus : 0].r); input = 1; }
        else if (d == 1) {
            struct gfx_rect ir = gfxterm_input_rect();
            if (gfx_rect_valid(ir)) gfx_damage(ir);
            else gfx_damage(G_win[G_focus >= 0 ? G_focus : 0].r);
            input = 1;
        }
        if (input || frame_due) gfx_present();

        if (real_input) last_input_tick = tick;
        u64 idle_ticks = tick - last_input_tick;
        u32 want = idle_ticks < 2  ? WAKE_ACTIVE_HZ
                 : idle_ticks < 20 ? WAKE_IDLE_HZ
                 :                    WAKE_DEEP_HZ;
        if (want != cur_wake) { timer_set_wake_hz(want); cur_wake = want; }
        if (!input) {
            res_wfi_inc();
            __asm__ volatile("wfi");
        }
    }

    timer_set_wake_hz(10);
    g_desktop_active = 0;
    gfxterm_stop();
    gfx_release_screen();
    fb_console_reset();
    kputs("[desktop] 已返回串口终端（Ctrl+Shift+F1 可再按一次）\n");
}
