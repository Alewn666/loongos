/* ============================================================================
 * macdesktop.c —— LoongOS 上的 macOS 风格桌面外壳（渲染 + 输入适配）
 * ----------------------------------------------------------------------------
 * 逻辑内核见 macwm.c/.h（57 条宿主单测通过）。本文件只做两件事：
 *   1) 把 mac_state 用 gfx 原语画出来：三段菜单栏 / Dock / 窗口三色按钮；
 *   2) 把键鼠输入翻译成 mac_dispatch 事件，更新 active_app 后重绘。
 * 独立入口 macdesktop_run()，由 `macdesktop` 命令启动，不碰原 desktop.c。
 * ==========================================================================*/
#include "loongos.h"
#include "gfx_draw.h"
#include "macwm.h"

/* ---- 配色 ---- */
#define MC_BG       0x1c1c1e
#define MC_WALL     0x2b3a4a
#define MC_MENUBG   0xF2F2F2
#define MC_MENUFG   0x1c1c1e
#define MC_WINTITLE 0xECECEC
#define MC_WINTXT   0x2b2b2b
#define MC_WINBD    0x6e6e73
#define MC_DOCKBG   0x9a9a9a
#define MC_DOT      0x1c1c1e
#define MC_RED      0xFF5F57
#define MC_YELLOW   0xFEBC2E
#define MC_GREEN    0x28C840
#define MC_ACCENT   0x1e9cff
#define MC_WHITE    0xFFFFFF
#define MC_PANEL    0xF7F7F7

/* ---- 布局 ---- */
#define MBAR_H    26
#define DOCK_H    66
#define TITLE_H   26
#define DOCK_ICON 42
#define DOCK_GAP  6

static struct mac_state g_ms;
static int g_inited;
static int g_mx, g_my;
static int g_menu_drawn;      /* 当前是否展开下拉菜单（苹果/App/Window） */
static int g_menu_kind;       /* 1=apple 2=app 3=window */

/* 颜色按 App 主题色（rodata，少用） */
static u32 app_tint(int app) {
    switch (app) {
        case MA_SAFARI: return 0x1e9cff;
        case MA_MAIL:   return 0x007aff;
        case MA_PHOTOS: return 0xff375f;
        case MA_MUSIC:  return 0xfa2d48;
        case MA_NOTES:  return 0xffcc00;
        case MA_CALC:   return 0xff9f0a;
        case MA_TERMINAL:   return 0x2c2c2e;
        case MA_SETTINGS: return 0x8e8e93;
        case MA_LAUNCHPAD: return 0x8e8e93;
        default: return 0x1e9cff;
    }
}

static void fill(u32 c) { struct gfx_rect r = {0,0,gfx_width(),gfx_height()}; gfx_fill(r,c); }
static struct gfx_rect full(void) { struct gfx_rect r = {0,0,gfx_width(),gfx_height()}; return r; }

/* ---- 三段菜单栏 ---- */
static void draw_menubar(void) {
    int W = gfx_width();
    struct gfx_rect bar = {0,0,W,MBAR_H};
    gfx_fill(bar, MC_MENUBG);
    /* 左：苹果菜单（固定，永远不随 App 变） */
    gfx_text(12, 5, "\x01", MC_MENUFG, 0xFFFFFFFF);   /* 用方块近似 Apple 标 */
    gfx_text(26, 5, mac_app_en(MA_FINDER), MC_MENUFG, 0xFFFFFFFF);
    /* 中：跟随 active_app 的 App 专属菜单 */
    const char *menus[16];
    int n = mac_menu_for_active(&g_ms, menus, 16);
    int x = 170;
    int active_bold = (g_ms.active_app == MA_FINDER);
    const char *appname = mac_app_en(g_ms.active_app);
    gfx_text(x, 5, appname, active_bold ? 0x000000 : 0x1c1c1e, 0xFFFFFFFF);
    x += gfx_text_w(appname) + 16;
    for (int i = 0; i < n; i++) {
        gfx_text(x, 5, menus[i], MC_MENUFG, 0xFFFFFFFF);
        x += gfx_text_w(menus[i]) + 14;
    }
    /* 右：常驻状态栏（时间/网络/音量/亮度/聚焦入口），不受 App 切换影响。
       从右边缘反向累进，避免与中间菜单重叠。 */
    const char *status[] = {"12:00", "Q", "Wi-Fi", "100%"};
    int sx = W - 8;
    for (int i = 0; i < 4; i++) {
        int w = gfx_text_w(status[i]);
        sx -= w;
        gfx_text(sx, 5, status[i], MC_MENUFG, 0xFFFFFFFF);
        sx -= 12;
    }
}

/* ---- 窗口 ---- */
static void draw_windows(void) {
    int H = gfx_height();
    /* 先画普通/最小化恢复的窗口，按 z 升序（小的先画） */
    for (int pass = 0; pass < MAC_NWIN; pass++) {
        int pick = -1;
        for (int i = 0; i < MAC_NWIN; i++) {
            if (!g_ms.wins[i].used) continue;
            if (g_ms.wins[i].state != MW_NORMAL && g_ms.wins[i].state != MW_FULL) continue;
            if (g_ms.wins[i].hidden_by_app) continue;
            if (g_ms.wins[i].z != pass + 1) continue;
            pick = i; break;
        }
        if (pick < 0) continue;
        struct mac_win *w = &g_ms.wins[pick];
        struct gfx_rect r = {w->b.x, w->b.y, w->b.w, w->b.h};
        if (w->state == MW_FULL) { r.x=0; r.y=MBAR_H; r.w=gfx_width(); r.h=H-MBAR_H; }
        /* 阴影边框 */
        struct gfx_rect bd = {r.x-2,r.y-2,r.w+4,r.h+4};
        gfx_rect_draw(bd, MC_WINBD, 1);
        /* 标题栏 */
        struct gfx_rect title = {r.x,r.y,r.w,TITLE_H};
        gfx_fill(title, (pick==g_ms.focused_win)?MC_WINTITLE:0xDADADA);
        /* 红黄绿三色按钮 */
        gfx_rect_draw((struct gfx_rect){r.x+10,r.y+8,12,12}, MC_RED, 12);
        gfx_rect_draw((struct gfx_rect){r.x+30,r.y+8,12,12}, MC_YELLOW, 12);
        gfx_rect_draw((struct gfx_rect){r.x+50,r.y+8,12,12}, MC_GREEN, 12);
        /* 标题文字 */
        const char *nm = mac_app_en(w->app);
        gfx_text(r.x+72, r.y+6, nm, MC_WINTXT, 0xFFFFFFFF);
        /* 内容区：App 主题底色 + 名称标识 */
        struct gfx_rect cr = {r.x+1,r.y+TITLE_H,r.w-2,r.h-TITLE_H-1};
        gfx_fill(cr, 0xFBFBFB);
        gfx_rect_draw(cr, app_tint(w->app), 1);
        gfx_text(cr.x+16, cr.y+16, nm, MC_WINTXT, 0xFFFFFFFF);
        gfx_text(cr.x+16, cr.y+36, "LoongOS", app_tint(w->app), 0xFFFFFFFF);
    }
}

/* ---- Dock ---- */
static int dock_x0(void) { return 8; }
static int dock_y0(void) { return gfx_height() - DOCK_H + 8; }

static void draw_dock(void) {
    int H = gfx_height(), W = gfx_width();
    struct gfx_rect dock = {0,H-DOCK_H,W,DOCK_H};
    gfx_fill(dock, MC_DOCKBG);
    gfx_rect_draw((struct gfx_rect){0,H-DOCK_H,W,2}, 0x6e6e73, 2);
    int x = dock_x0(), y = dock_y0();
    /* 左+中：App 图标（按 App 编号顺序） */
    for (int a = 0; a < MA_N; a++) {
        if (!mac_dock_has(&g_ms, a)) continue;
        struct gfx_rect ico = {x,y,DOCK_ICON,DOCK_ICON};
        gfx_fill(ico, app_tint(a));
        gfx_rect_draw(ico, 0xFFFFFF, 1);
        /* 用 App 英文名首字母作图标（无 emoji 字体也能辨识） */
        char lab[2]; lab[0] = mac_app_en(a)[0]; lab[1] = 0;
        gfx_text(x+16, y+12, lab, MC_WHITE, 0xFFFFFFFF);
        /* 运行圆点 */
        if (mac_app_running(&g_ms, a)) {
            struct gfx_rect dot = {x+DOCK_ICON/2-3, y+DOCK_ICON+4, 6, 6};
            gfx_fill(dot, MC_DOT);
        }
        x += DOCK_ICON + DOCK_GAP;
    }
    /* 右侧：最小化窗口缩略图 */
    int mins[16], mnc = mac_minimized_wins(&g_ms, mins, 16);
    for (int i = 0; i < mnc; i++) {
        int a = mac_win_app(&g_ms, mins[i]);
        struct gfx_rect ico = {x,y,DOCK_ICON,DOCK_ICON};
        gfx_fill(ico, app_tint(a));
        gfx_rect_draw(ico, MC_WHITE, 1);
        x += DOCK_ICON + DOCK_GAP;
    }
    /* 废纸篓（Dock 最右固定） */
    struct gfx_rect trash = {W-DOCK_ICON-8,y,DOCK_ICON,DOCK_ICON};
    gfx_fill(trash, 0x8e8e93);
    gfx_rect_draw(trash, MC_WHITE, 1);
    gfx_text(W-DOCK_ICON+6, y+12, "T", MC_WHITE, 0xFFFFFFFF);
}

/* ---- 下拉菜单（苹果 / App / Window / Spotlight 简化覆盖层） ---- */
static void draw_overlays(void) {
    if (!g_menu_drawn) return;
    struct gfx_rect m = {g_menu_kind==1?8:(g_menu_kind==3?420:170), MBAR_H, 260, 220};
    gfx_fill(m, MC_PANEL);
    gfx_rect_draw(m, MC_WINBD, 1);
    if (g_menu_kind == 1) {
        static const char *const items[8] = {"关于本机","系统设置...","最近使用项目","强制退出...","睡眠","重新启动","关机","锁屏"};
        for (int i=0;i<8;i++) gfx_text(m.x+12, m.y+10+i*24, items[i], MC_MENUFG, 0xFFFFFFFF);
    } else if (g_menu_kind == 3) {
        int wins[16], n = mac_window_menu_wins(&g_ms, wins, 16);
        for (int i=0;i<n;i++) gfx_text(m.x+12, m.y+10+i*22, mac_app_en(mac_win_app(&g_ms,wins[i])), MC_MENUFG, 0xFFFFFFFF);
        int y = m.y+10+n*22+6;
        gfx_text(m.x+12, y, "最小化", MC_MENUFG, 0xFFFFFFFF);
        gfx_text(m.x+12, y+22, "前置全部窗口", MC_MENUFG, 0xFFFFFFFF);
        gfx_text(m.x+12, y+44, "全屏", MC_MENUFG, 0xFFFFFFFF);
    } else {
        const char *menus[16];
        int n = mac_menu_for_active(&g_ms, menus, 16);
        for (int i=0;i<n;i++) gfx_text(m.x+12, m.y+10+i*24, menus[i], MC_MENUFG, 0xFFFFFFFF);
    }
}

static void mac_redraw(struct gfx_rect r) {
    (void)r;
    fill(MC_WALL);
    draw_windows();
    draw_menubar();
    draw_dock();
    draw_overlays();
}

/* ---- 命中测试 ---- */
static int hit(struct gfx_rect r, int x, int y) {
    return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;
}
static int dock_app_at(int x, int y) {
    int cx = dock_x0(), cy = dock_y0();
    for (int a = 0; a < MA_N; a++) {
        if (!mac_dock_has(&g_ms, a)) continue;
        if (hit((struct gfx_rect){cx,cy,DOCK_ICON,DOCK_ICON}, x, y)) return a;
        cx += DOCK_ICON + DOCK_GAP;
    }
    return -1;
}
static int min_thumb_at(int x, int y) {
    int mins[16], mnc = mac_minimized_wins(&g_ms, mins, 16);
    int cx = dock_x0();
    for (int a = 0; a < MA_N; a++) if (mac_dock_has(&g_ms,a)) cx += DOCK_ICON+DOCK_GAP;
    for (int i = 0; i < mnc; i++) {
        if (hit((struct gfx_rect){cx,dock_y0(),DOCK_ICON,DOCK_ICON}, x, y)) return mins[i];
        cx += DOCK_ICON + DOCK_GAP;
    }
    return -1;
}
/* 找点击命中的最顶层窗口 */
static int top_win_at(int x, int y) {
    int best = -1, bz = -1;
    for (int i = 0; i < MAC_NWIN; i++) {
        if (!g_ms.wins[i].used || g_ms.wins[i].hidden_by_app) continue;
        if (g_ms.wins[i].state != MW_NORMAL && g_ms.wins[i].state != MW_FULL) continue;
        struct gfx_rect r = {g_ms.wins[i].b.x, g_ms.wins[i].b.y, g_ms.wins[i].b.w, g_ms.wins[i].b.h};
        if (g_ms.wins[i].state == MW_FULL) { r.x=0;r.y=MBAR_H;r.w=gfx_width();r.h=gfx_height()-MBAR_H; }
        if (!hit(r,x,y)) continue;
        if (g_ms.wins[i].z > bz) { bz = g_ms.wins[i].z; best = i; }
    }
    return best;
}

static void on_press(int x, int y) {
    /* 1. 下拉菜单打开时优先吃点击 */
    if (g_menu_drawn) {
        if (g_menu_kind == 3) {
            struct gfx_rect m = {420,MBAR_H,260,220};
            if (hit(m,x,y)) {
                /* 简化：菜单内上半为窗口列表，下半三按钮 */
                int wins[16], n = mac_window_menu_wins(&g_ms, wins, 16);
                int yy = y - (m.y+10);
                if (yy >= 0 && yy < n*22) mac_dispatch(&g_ms, ME_MENUCLICK, MACT_FOCUSWIN, wins[yy/22]);
                else {
                    int by = yy - (n*22+6);
                    if (by>=0 && by<22) mac_dispatch(&g_ms, ME_MENUCLICK, MACT_MINIMIZE, 0);
                    else if (by>=44 && by<66) mac_dispatch(&g_ms, ME_MENUCLICK, MACT_FRONTALL, 0);
                    else if (by>=88 && by<110) mac_dispatch(&g_ms, ME_MENUCLICK, MACT_FULLSCREEN, 0);
                }
            }
        }
        g_menu_drawn = 0;
        gfx_damage(full());
        return;
    }
    /* 2. 顶部菜单栏 */
    if (y < MBAR_H) {
        if (x < 150) { g_menu_drawn=1; g_menu_kind=1; mac_dispatch(&g_ms, ME_MENUOPEN, MMS_APPLE, 0); }
        else if (x >= gfx_width()-80) { mac_dispatch(&g_ms, ME_SPOTLIGHT_TOGGLE, 0, 1); }
        else if (x >= 400 && x < 680) { g_menu_drawn=1; g_menu_kind=3; mac_dispatch(&g_ms, ME_MENUOPEN, MMS_APP, 0); }
        else { g_menu_drawn=1; g_menu_kind=2; mac_dispatch(&g_ms, ME_MENUOPEN, MMS_APP, 0); }
        gfx_damage(full());
        return;
    }
    /* 3. Dock */
    if (y >= gfx_height()-DOCK_H) {
        int wi = min_thumb_at(x,y);
        if (wi >= 0) { mac_dispatch(&g_ms, ME_DOCKRESTORE, wi, 0); gfx_damage(full()); return; }
        int a = dock_app_at(x,y);
        if (a >= 0) { mac_dispatch(&g_ms, ME_DOCKCLICK, a, 0); gfx_damage(full()); return; }
        gfx_damage(full()); return;
    }
    /* 4. 窗口 */
    int wi = top_win_at(x,y);
    if (wi < 0) { mac_dispatch(&g_ms, ME_CLICKDESKTOP, 0,0); gfx_damage(full()); return; }
    struct gfx_rect r = {g_ms.wins[wi].b.x, g_ms.wins[wi].b.y, g_ms.wins[wi].b.w, g_ms.wins[wi].b.h};
    if (g_ms.wins[wi].state == MW_FULL) { r.x=0;r.y=MBAR_H;r.w=gfx_width();r.h=gfx_height()-MBAR_H; }
    if (y < r.y+TITLE_H) {
        if (x>=r.x+10 && x<r.x+22)      mac_dispatch(&g_ms, ME_CLOSEWIN, wi, 0);       /* 红 */
        else if (x>=r.x+30 && x<r.x+42) mac_dispatch(&g_ms, ME_MINWIN, wi, 0);         /* 黄 */
        else if (x>=r.x+50 && x<r.x+62) mac_dispatch(&g_ms, ME_FULLSCREEN, wi, 0);     /* 绿 */
        else                            mac_dispatch(&g_ms, ME_FOCUSWIN, wi, 0);
    } else mac_dispatch(&g_ms, ME_FOCUSWIN, wi, 0);
    gfx_damage(full());
}

/* ---- 主循环 ---- */
void macdesktop_run(void) {
    if (!gfx_ready()) { kputs("[macdesktop] 无帧缓冲，保持串口 shell\n"); return; }
    gfx_take_screen();
    gfx_set_redraw(mac_redraw);
    if (!g_inited) {
        mac_init(&g_ms);
        /* 默认打开 Safari/备忘录/访达 三个窗口，错开摆放便于辨识层级 */
        mac_dispatch(&g_ms, ME_LAUNCH, MA_SAFARI, 0);
        mac_dispatch(&g_ms, ME_LAUNCH, MA_NOTES, 0);
        mac_dispatch(&g_ms, ME_LAUNCH, MA_FINDER, 0);
        /* 按 App 错开窗口位置（macwm 默认首窗都在同一坐标） */
        for (int i = 0; i < MAC_NWIN; i++) {
            if (!g_ms.wins[i].used) continue;
            int a = g_ms.wins[i].app;
            g_ms.wins[i].b.x = (short)(60 + (a%3)*70);
            g_ms.wins[i].b.y = (short)(50 + (a%3)*50);
            g_ms.wins[i].b.w = 560; g_ms.wins[i].b.h = 380;
        }
        g_mx = gfx_width()/2; g_my = gfx_height()/2;
        g_inited = 1;
    }
    gfx_damage(full());
    gfx_present();
    kputs("[macdesktop] macOS 风格桌面已就绪\n");
    kputs("[macdesktop] 点窗口激活 App、点桌面回访达、点 Dock 图标切换/启动\n");
    kputs("[macdesktop] 红=关窗 黄=最小化 绿=全屏；q/串口输入退出\n");

    u64 freq = 24000000ull;
    u64 frame_cyc = freq/60; if(!frame_cyc) frame_cyc=1;
    u64 next_frame = 0; /* 0 = 首帧后立即重绘 */
    for (;;) {
        usb_kbd_poll();
        if (desktop_ring_peek_uart()) break;
        if (desktop_exit_requested()) break;
        char c;
        while (desktop_ring_pop_kbd(&c)) {
            if (c=='q' || c=='Q' || c==3) goto done;
            /* Cmd(以 Ctrl 代)快捷键: Tab 切 App, 空格 Spotlight, w 关窗, m 最小化 */
            if (c=='\t') { mac_dispatch(&g_ms, ME_CMDTAB, 0, 0); gfx_damage(full()); }
            else if (c==' ') { mac_dispatch(&g_ms, ME_SPOTLIGHT_TOGGLE, 0, 1); gfx_damage(full()); }
            else if (c=='w'||c=='W') { if(g_ms.focused_win>=0) mac_dispatch(&g_ms, ME_CLOSEWIN, g_ms.focused_win,0); gfx_damage(full()); }
            else if (c=='m'||c=='M') { if(g_ms.focused_win>=0) mac_dispatch(&g_ms, ME_MINWIN, g_ms.focused_win,0); gfx_damage(full()); }
        }
        /* 鼠标 */
        int btn=0, wheel=0;
        mouse_get(&g_mx,&g_my,&btn,&wheel);
        int st=0;
        if (mouse_pop_btn(&st) && (st&1)) on_press(g_mx,g_my);
        gfx_present();
        (void)frame_cyc; (void)next_frame;
    }
done:
    gfxterm_stop();
    g_inited = 1;
    fb_console_reset();
}
