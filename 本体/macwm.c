/* ============================================================================
 * macwm.c —— macOS 桌面交互内核实现（纯逻辑 / 宿主与内核共用）
 * 与 MacDesktop复刻/js/wm_core.js 语义一一对应，交互规则严格对齐 macOS。
 * ==========================================================================*/
#include "macwm.h"

/* ---- 无标准库依赖的小工具（宿主/内核通用） ---- */
static int mac_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int mac_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
/* 不区分大小写的子串查找：命中返回 1 */
static int mac_ihas(const char *hay, const char *needle) {
    int nl = mac_len(needle);
    if (nl == 0) return 1;
    for (const char *p = hay; *p; p++) {
        int k = 0;
        while (k < nl && p[k] && mac_lower((unsigned char)p[k]) == mac_lower((unsigned char)needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

/* ---- 菜单表（rodata） ---- */
static const char *const M_FINDER[]  = {"File","Edit","View","Go","Window","Help"};
static const char *const M_LAUNCH[]  = {"File","Edit","View","Window","Help"};
static const char *const M_SAFARI[]  = {"File","Edit","View","History","Bookmarks","Window","Help"};
static const char *const M_NOTES[]   = {"File","Edit","View","Format","Window","Help"};
static const char *const M_TERM[]    = {"Shell","Edit","View","Window","Help"};
static const char *const M_CALC[]    = {"File","Edit","View","Window","Help"};
static const char *const M_PHOTOS[]  = {"File","Edit","View","Window","Help"};
static const char *const M_MUSIC[]   = {"File","Edit","View","Controls","Window","Help"};
static const char *const M_SET[]     = {"File","Edit","View","Window","Help"};
static const char *const M_MAIL[]    = {"File","Edit","View","Go","Window","Help"};

const struct mac_appdef mac_defs[MA_N] = {
    {"访达",       "Finder",          M_FINDER, 6, 1, 0, 1},
    {"启动台",     "Launchpad",       M_LAUNCH, 5, 1, 1, 1},
    {"Safari 浏览器","Safari",        M_SAFARI, 7, 0, 0, 2},
    {"备忘录",     "Notes",           M_NOTES,  6, 0, 0, 2},
    {"终端",       "Terminal",        M_TERM,   5, 0, 0, 2},
    {"计算器",     "Calculator",      M_CALC,   5, 0, 0, 2},
    {"照片",       "Photos",          M_PHOTOS, 5, 0, 0, 2},
    {"音乐",       "Music",           M_MUSIC,  6, 0, 0, 2},
    {"系统设置",   "System Settings", M_SET,    5, 0, 0, 2},
    {"邮件",       "Mail",            M_MAIL,   6, 0, 0, 2}
};

const char *mac_app_cn(int app) { return (app >= 0 && app < MA_N) ? mac_defs[app].cn : ""; }
const char *mac_app_en(int app) { return (app >= 0 && app < MA_N) ? mac_defs[app].en : ""; }

/* ---- 内部工具 ---- */
static void touch_mru(struct mac_state *st, int app) {
    int i, at = -1;
    for (i = 0; i < st->mru_n; i++) if (st->mru[i] == app) { at = i; break; }
    if (at == 0) return;
    if (at > 0) { for (i = at; i < st->mru_n - 1; i++) st->mru[i] = st->mru[i+1]; st->mru_n--; }
    if (st->mru_n < MA_N) { for (i = st->mru_n; i > 0; i--) st->mru[i] = st->mru[i-1]; st->mru[0] = app; st->mru_n++; }
}
static void bcopy(struct mac_bounds *d, const struct mac_bounds *s) {
    d->x=s->x; d->y=s->y; d->w=s->w; d->h=s->h;
}
static int find_free_win(struct mac_state *st) {
    for (int i = 0; i < MAC_NWIN; i++) if (!st->wins[i].used) return i;
    return -1;
}
static int find_free_space(struct mac_state *st) {
    for (int i = 1; i < MAC_NSPACE; i++) if (!st->spaces[i].used) return i;
    return -1;
}
static int win_visible(const struct mac_state *st, int wi) {
    return wi >= 0 && wi < MAC_NWIN && st->wins[wi].used &&
           st->wins[wi].state == MW_NORMAL && !st->wins[wi].hidden_by_app;
}
/* App 内最合适的焦点窗口：优先当前 space 的普通窗口，取 z 最高 */
static int pick_focused(struct mac_state *st, int app) {
    int best = -1, bestz = -1, best_here = -1, best_here_z = -1;
    if (app < 0 || app >= MA_N) return -1;
    for (int i = 0; i < MAC_NWIN; i++) {
        if (!(st->apps[app].winmask & (1u << i))) continue;
        if (!win_visible(st, i)) continue;
        if (st->wins[i].z > bestz) { bestz = st->wins[i].z; best = i; }
        if (st->wins[i].space == st->cur_space && st->wins[i].z > best_here_z) {
            best_here_z = st->wins[i].z; best_here = i;
        }
    }
    return best_here >= 0 ? best_here : best;
}
static int visible_count(const struct mac_state *st, int app) {
    int n = 0;
    if (app < 0 || app >= MA_N) return 0;
    for (int i = 0; i < MAC_NWIN; i++)
        if ((st->apps[app].winmask & (1u << i)) && win_visible(st, i)) n++;
    return n;
}
static int win_count(const struct mac_state *st, int app) {
    if (app < 0 || app >= MA_N) return 0;
    int n = 0;
    for (int i = 0; i < MAC_NWIN; i++) if (st->apps[app].winmask & (1u << i)) n++;
    return n;
}
static int prev_running_except(struct mac_state *st, int except_id) {
    for (int i = 0; i < st->mru_n; i++) {
        int x = st->mru[i];
        if (x != except_id && st->apps[x].running && !st->apps[x].hidden) return x;
    }
    return MA_FINDER;
}

/* ---- 前置声明 ---- */
static void do_activate(struct mac_state *st, int app);
static void do_new_window(struct mac_state *st, int app);
static void do_restore(struct mac_state *st, int wi);
static void do_space_switch(struct mac_state *st, int sp);

void mac_init(struct mac_state *st) {
    int i;
    for (i = 0; i < sizeof(struct mac_state); i++) ((unsigned char *)st)[i] = 0;
    for (i = 0; i < MA_N; i++) {
        st->dock_mask |= (1u << i);   /* 所有 App 默认固定在 Dock */
    }
    /* Finder 常驻：开机即运行（桌面是它的画布） */
    st->apps[MA_FINDER].running = 1;
    st->apps[MA_FINDER].pid = 100;
    st->mru[0] = MA_FINDER; st->mru_n = 1;
    st->seq_pid = 101; st->seq_win = 1; st->seq_space = 1;
    st->spaces[0].used = 1; st->spaces[0].type = MST_DESKTOP;
    st->cur_space = 0;
    st->active_app = MA_FINDER;
    st->focused_win = -1;
}

/* ---- App 生命周期 ---- */
static void do_launch(struct mac_state *st, int app, int no_window) {
    if (app < 0 || app >= MA_N) return;
    if (!st->apps[app].running) {
        st->apps[app].running = 1;
        st->apps[app].pid = st->seq_pid++;
        st->apps[app].hidden = 0;
        st->apps[app].launch_ts = st->t;
        touch_mru(st, app);
    }
    if (!no_window && !mac_defs[app].no_window && win_count(st, app) == 0)
        do_new_window(st, app);
    do_activate(st, app);
}
static void do_quit(struct mac_state *st, int app, int force) {
    if (app < 0 || app >= MA_N || !st->apps[app].running) return;
    if (mac_defs[app].persistent && !force) return;
    for (int i = 0; i < MAC_NWIN; i++) {
        if (st->apps[app].winmask & (1u << i)) {
            st->wins[i].used = 0;
            st->apps[app].winmask &= ~(1u << i);
            int sp = st->wins[i].space;
            if (sp >= 0 && sp < MAC_NSPACE) st->spaces[sp].winmask &= ~(1u << i);
        }
    }
    st->apps[app].running = 0;
    st->apps[app].pid = 0;
    st->apps[app].hidden = 0;
    for (int i = 0; i < st->mru_n; i++) if (st->mru[i] == app) {
        for (int k = i; k < st->mru_n - 1; k++) st->mru[k] = st->mru[k+1];
        st->mru_n--; break;
    }
    if (st->active_app == app) do_activate(st, MA_FINDER);
}
static void do_activate(struct mac_state *st, int app) {
    if (app < 0 || app >= MA_N) return;
    if (!st->apps[app].running) { do_launch(st, app, 0); return; }
    st->apps[app].hidden = 0;
    /* 若该 App 有全屏窗口且不在当前 space，切到那个 space */
    for (int i = 0; i < MAC_NWIN; i++) {
        if ((st->apps[app].winmask & (1u << i)) && st->wins[i].used &&
            st->wins[i].state == MW_FULL && st->wins[i].space != st->cur_space) {
            do_space_switch(st, st->wins[i].space); break;
        }
    }
    st->active_app = app;              /* ★ 菜单栏中间段绑定此值 */
    st->focused_win = pick_focused(st, app);
    touch_mru(st, app);
    st->menu_open = 0;
}
static void do_hide(struct mac_state *st, int app) {
    if (app < 0 || app >= MA_N || !st->apps[app].running || mac_defs[app].persistent) return;
    st->apps[app].hidden = 1;
    for (int i = 0; i < MAC_NWIN; i++)
        if (st->apps[app].winmask & (1u << i)) st->wins[i].hidden_by_app = 1;
    if (st->active_app == app) do_activate(st, prev_running_except(st, app));
}

/* ---- 窗口生命周期（与 App 解耦） ---- */
static void do_new_window(struct mac_state *st, int app) {
    if (app < 0 || app >= MA_N || !st->apps[app].running) return;
    int wi = find_free_win(st);
    if (wi < 0) return;
    struct mac_win *w = &st->wins[wi];
    int n = win_count(st, app);
    w->used = 1; w->app = (unsigned char)app; w->state = MW_NORMAL;
    w->space = (unsigned char)st->cur_space; w->hidden_by_app = 0;
    int k = n % 6;
    w->b.x = (short)(90 + k * 34); w->b.y = (short)(60 + k * 26);
    w->b.w = 640; w->b.h = 430;
    w->restore.x = w->restore.y = w->restore.w = w->restore.h = 0;
    w->z = ++st->ztop;
    st->apps[app].winmask |= (1u << wi);
    if (st->cur_space >= 0 && st->cur_space < MAC_NSPACE)
        st->spaces[st->cur_space].winmask |= (1u << wi);
    st->focused_win = wi;
    st->active_app = app;
    touch_mru(st, app);
}
static void destroy_win(struct mac_state *st, int wi) {
    if (wi < 0 || wi >= MAC_NWIN || !st->wins[wi].used) return;
    int app = st->wins[wi].app, sp = st->wins[wi].space;
    if (app >= 0 && app < MA_N) st->apps[app].winmask &= ~(1u << wi);
    if (sp >= 0 && sp < MAC_NSPACE) st->spaces[sp].winmask &= ~(1u << wi);
    st->wins[wi].used = 0;
}
static void do_close_win(struct mac_state *st, int wi) {
    if (wi < 0 || wi >= MAC_NWIN || !st->wins[wi].used) return;
    int app = st->wins[wi].app;
    destroy_win(st, wi);
    if (st->focused_win == wi) st->focused_win = pick_focused(st, app);
    /* ★ active_app 不变：关掉最后一个窗口，App 仍在后台、菜单栏仍是它 */
}
static void do_focus_win(struct mac_state *st, int wi) {
    if (wi < 0 || wi >= MAC_NWIN || !st->wins[wi].used) return;
    do_activate(st, st->wins[wi].app);       /* 点窗口 → 激活其 App → 菜单栏刷新 */
    if (st->wins[wi].state == MW_MIN) do_restore(st, wi);
    if (st->wins[wi].space != st->cur_space) do_space_switch(st, st->wins[wi].space);
    st->wins[wi].z = ++st->ztop;
    st->focused_win = (short)wi;
    st->menu_open = 0;
}
static void exit_fullscreen(struct mac_state *st, int wi) {
    struct mac_win *w = &st->wins[wi];
    int sp = w->space;
    if (sp > 0 && sp < MAC_NSPACE && st->spaces[sp].type == MST_FULL) {
        st->spaces[sp].used = 0; st->spaces[sp].winmask = 0;
    }
    w->space = 0;
    st->spaces[0].winmask |= (1u << wi);
    w->state = MW_NORMAL;
    if (w->restore.w > 0) { bcopy(&w->b,&w->restore); w->restore.x = w->restore.y = w->restore.w = w->restore.h = 0; }
    if (st->cur_space == sp) st->cur_space = 0;
}
static void enter_fullscreen(struct mac_state *st, int wi) {
    struct mac_win *w = &st->wins[wi];
    int sp = find_free_space(st);
    if (sp < 0) return;
    st->spaces[sp].used = 1; st->spaces[sp].type = MST_FULL;
    st->spaces[sp].app = w->app; st->spaces[sp].winmask = (1u << wi);
    int old = w->space;
    if (old >= 0 && old < MAC_NSPACE) st->spaces[old].winmask &= ~(1u << wi);
    bcopy(&w->restore,&w->b);
    w->state = MW_FULL;
    w->space = (unsigned char)sp;
    st->cur_space = (short)sp;                /* 全屏 App 独占并切入该 Space */
    st->active_app = w->app;
    st->focused_win = (short)wi;
}
static void do_minimize(struct mac_state *st, int wi) {
    if (wi < 0 || wi >= MAC_NWIN || !st->wins[wi].used || st->wins[wi].state == MW_MIN) return;
    int app = st->wins[wi].app;
    if (st->wins[wi].state == MW_FULL) exit_fullscreen(st, wi);
    st->wins[wi].state = MW_MIN;
    if (st->focused_win == wi) st->focused_win = pick_focused(st, app);
}
static void do_restore(struct mac_state *st, int wi) {
    if (wi < 0 || wi >= MAC_NWIN || !st->wins[wi].used) return;
    st->wins[wi].state = MW_NORMAL;
    st->wins[wi].hidden_by_app = 0;
    if (st->wins[wi].app < MA_N) st->apps[st->wins[wi].app].hidden = 0;
    if (st->wins[wi].space != st->cur_space) do_space_switch(st, st->wins[wi].space);
    st->wins[wi].z = ++st->ztop;
    do_activate(st, st->wins[wi].app);
    st->focused_win = (short)wi;
}
static void do_fullscreen(struct mac_state *st, int wi, int option) {
    if (wi < 0 || wi >= MAC_NWIN || !st->wins[wi].used) return;
    struct mac_win *w = &st->wins[wi];
    if (option) {   /* Option + 绿：普通 zoom，不建 Space */
        if (w->restore.w > 0) { bcopy(&w->b,&w->restore); w->restore.x=w->restore.y=w->restore.w=w->restore.h=0; }
        else { bcopy(&w->restore,&w->b); w->b.x=0; w->b.y=24; w->b.w=1440; w->b.h=900; }
        return;
    }
    if (w->state == MW_FULL) exit_fullscreen(st, wi);
    else enter_fullscreen(st, wi);
}

/* ---- Dock ---- */
static void do_dock_click(struct mac_state *st, int app) {
    if (app < 0 || app >= MA_N) return;
    if (!st->apps[app].running) { do_launch(st, app, 0); return; }
    if (st->apps[app].hidden) {
        st->apps[app].hidden = 0;
        for (int i = 0; i < MAC_NWIN; i++)
            if (st->apps[app].winmask & (1u << i)) st->wins[i].hidden_by_app = 0;
    }
    int vc = visible_count(st, app);
    if (win_count(st, app) > 0 && vc == 0) {   /* 全部最小化 → 恢复 */
        for (int i = 0; i < MAC_NWIN; i++)
            if ((st->apps[app].winmask & (1u << i)) && st->wins[i].state == MW_MIN)
                do_restore(st, i);
    }
    do_activate(st, app);
    for (int i = 0; i < MAC_NWIN; i++)
        if ((st->apps[app].winmask & (1u << i)) && win_visible(st, i)) st->wins[i].z = ++st->ztop;
    st->focused_win = pick_focused(st, app);
}
static void do_dock_add(struct mac_state *st, int app) {
    if (app >= 0 && app < MA_N) st->dock_mask |= (1u << app);
}
static void do_dock_remove(struct mac_state *st, int app) {
    if (mac_defs[app].dock_zone == 1) return;  /* 左侧常驻项不可移除 */
    if (app >= 0 && app < MA_N) st->dock_mask &= ~(1u << app);
}

/* ---- 焦点切换 / Space ---- */
static void do_cmdtab(struct mac_state *st, int backward) {
    int order[MA_N], n = 0;
    for (int i = 0; i < st->mru_n; i++) {
        int x = st->mru[i];
        if (st->apps[x].running && !st->apps[x].hidden) order[n++] = x;
    }
    if (n == 0) return;
    if (n == 1) { do_activate(st, order[0]); return; }
    int at = 0;
    for (int i = 0; i < n; i++) if (order[i] == st->active_app) { at = i; break; }
    int next = backward ? (at - 1 + n) % n : (at + 1) % n;
    do_activate(st, order[next]);
}
static void do_space_switch(struct mac_state *st, int sp) {
    if (sp < 0 || sp >= MAC_NSPACE || !st->spaces[sp].used) return;
    st->cur_space = (short)sp;
    if (st->spaces[sp].type == MST_FULL && st->spaces[sp].app < MA_N)
        do_activate(st, st->spaces[sp].app);
    else if (st->spaces[sp].winmask) {
        for (int i = MAC_NWIN - 1; i >= 0; i--)
            if (st->spaces[sp].winmask & (1u << i)) { do_activate(st, st->wins[i].app); break; }
    } else do_activate(st, MA_FINDER);
}

/* ---- 菜单点击 ---- */
static void do_menu_click(struct mac_state *st, int action, int win) {
    st->menu_open = 0;
    switch (action) {
        case MACT_SETTINGS: do_launch(st, MA_SETTINGS, 0); break;
        case MACT_FORCEQUIT: st->forcequit_open = 1; break;
        case MACT_NEWWINDOW: do_new_window(st, st->active_app); break;
        case MACT_CLOSEWINDOW: if (st->focused_win >= 0) do_close_win(st, st->focused_win); break;
        case MACT_MINIMIZE:    if (st->focused_win >= 0) do_minimize(st, st->focused_win); break;
        case MACT_FULLSCREEN:  if (st->focused_win >= 0) do_fullscreen(st, st->focused_win, 0); break;
        case MACT_HIDE: do_hide(st, st->active_app); break;
        case MACT_QUIT: do_quit(st, st->active_app, 0); break;
        case MACT_FRONTALL: {
            for (int i = 0; i < MAC_NWIN; i++)
                if ((st->apps[st->active_app].winmask & (1u << i)) && win_visible(st, i)) st->wins[i].z = ++st->ztop;
            st->focused_win = pick_focused(st, st->active_app);
            break;
        }
        case MACT_FOCUSWIN: if (win >= 0) do_focus_win(st, win); break;
        case MACT_SPOTLIGHT: st->spotlight_open = 1; break;
        case MACT_LAUNCHPAD: st->launchpad_open = 1; break;
        default: break;  /* about/recent/sleep/restart/shutdown/lock/generic → UI 呈现 */
    }
}

/* ---- 事件分发 ---- */
void mac_dispatch(struct mac_state *st, int evt, int a, int b) {
    st->t++;
    st->last_evt = (unsigned char)evt;
    switch (evt) {
        case ME_LAUNCH:        do_launch(st, a, b); break;
        case ME_QUIT:          do_quit(st, a ? a : st->active_app, b); break;
        case ME_ACTIVATE:      do_activate(st, a); break;
        case ME_HIDE:          do_hide(st, a ? a : st->active_app); break;
        case ME_NEWWIN:        do_new_window(st, a ? a : st->active_app); break;
        case ME_FOCUSWIN:      do_focus_win(st, a); break;
        case ME_CLOSEWIN:      do_close_win(st, a); break;
        case ME_MINWIN:        do_minimize(st, a); break;
        case ME_RESTOREWIN:    do_restore(st, a); break;
        case ME_FULLSCREEN:    do_fullscreen(st, a, 0); break;
        case ME_ZOOM:          do_fullscreen(st, a, 1); break;
        case ME_DOCKCLICK:     do_dock_click(st, a); break;
        case ME_DOCKRESTORE:   do_restore(st, a); break;
        case ME_DOCKADD:       do_dock_add(st, a); break;
        case ME_DOCKREMOVE:    do_dock_remove(st, a); break;
        case ME_CMDTAB:        do_cmdtab(st, b); break;
        case ME_CLICKDESKTOP:  do_activate(st, MA_FINDER); break;
        case ME_MENUOPEN:      st->menu_open = 1; st->menu_which = (unsigned char)a; st->menu_app = (unsigned char)(a == MMS_APP ? (b ? b : st->active_app) : 0); break;
        case ME_MENUCLICK:     do_menu_click(st, a, b); break;
        case ME_SPOTLIGHT_TOGGLE:
            st->spotlight_open = (unsigned char)(b ? 1 : (a ? 1 : !st->spotlight_open));
            if (!st->spotlight_open) { /* query 由渲染层管理，内核不存长串 */ }
            st->menu_open = 0; break;
        case ME_LAUNCHPAD:     st->launchpad_open = (unsigned char)(a ? 1 : !st->launchpad_open); break;
        case ME_SPACE:         do_space_switch(st, a); break;
        case ME_FORCEQUIT:     st->forcequit_open = (unsigned char)(a ? 1 : !st->forcequit_open); break;
        default: break;
    }
}

/* ---- 派生查询 ---- */
int mac_app_running(struct mac_state *st, int app) { return app >= 0 && app < MA_N && st->apps[app].running; }
int mac_app_hidden(struct mac_state *st, int app) { return app >= 0 && app < MA_N && st->apps[app].hidden; }
int mac_app_pid(struct mac_state *st, int app) { return app >= 0 && app < MA_N ? st->apps[app].pid : 0; }
int mac_active(struct mac_state *st) { return st->active_app; }
int mac_focused(struct mac_state *st) { return st->focused_win; }
int mac_win_count(struct mac_state *st, int app) { return win_count(st, app); }
int mac_win_state(struct mac_state *st, int wi) { return wi >= 0 && wi < MAC_NWIN && st->wins[wi].used ? st->wins[wi].state : 0; }
int mac_win_app(struct mac_state *st, int wi) { return wi >= 0 && wi < MAC_NWIN && st->wins[wi].used ? st->wins[wi].app : -1; }
struct mac_bounds mac_win_bounds(struct mac_state *st, int wi) { struct mac_bounds r; bcopy(&r,&st->wins[wi].b); return r; }
int mac_menu_for_active(struct mac_state *st, const char **out, int cap) {
    int app = st->active_app;
    if (app < 0 || app >= MA_N) return 0;
    int n = mac_defs[app].nmenu, k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) out[i] = mac_defs[app].menus[i];
    return k;
}
int mac_window_menu_wins(struct mac_state *st, int *out, int cap) {
    int app = st->active_app, n = 0;
    if (app < 0 || app >= MA_N) return 0;
    for (int i = 0; i < MAC_NWIN && n < cap; i++)
        if ((st->apps[app].winmask & (1u << i)) && st->wins[i].used) out[n++] = i;
    return n;
}
int mac_dock_has(struct mac_state *st, int app) { return app >= 0 && app < MA_N && (st->dock_mask & (1u << app)); }
int mac_dock_zone(struct mac_state *st, int app) {
    (void)st;
    return app >= 0 && app < MA_N ? mac_defs[app].dock_zone : 0;
}
int mac_minimized_wins(struct mac_state *st, int *out, int cap) {
    int n = 0;
    for (int i = 0; i < MAC_NWIN && n < cap; i++)
        if (st->wins[i].used && st->wins[i].state == MW_MIN) out[n++] = i;
    return n;
}

/* ---- Spotlight 四则计算（递归下降，整数） ---- */
static const char *cp;
static int expr(int *ok);
static int factor(int *ok) {
    while (*cp == ' ') cp++;
    if (*cp == '(') { cp++; int v = expr(ok); while (*cp == ' ') cp++; if (*cp == ')') cp++; return v; }
    if (*cp == '-') { cp++; return -factor(ok); }
    int neg = 0, any = 0, v = 0;
    if (*cp == '+') cp++;
    while (*cp >= '0' && *cp <= '9') { v = v * 10 + (*cp - '0'); cp++; any = 1; }
    if (!any) { *ok = 0; return 0; }
    (void)neg;
    return v;
}
static int term(int *ok) {
    int v = factor(ok);
    for (;;) {
        while (*cp == ' ') cp++;
        if (*cp == '*') { cp++; int r = factor(ok); v = v * r; }
        else if (*cp == '/') { cp++; int r = factor(ok); if (r == 0) { *ok = 0; return 0; } v = v / r; }
        else break;
    }
    return v;
}
static int expr(int *ok) {
    int v = term(ok);
    for (;;) {
        while (*cp == ' ') cp++;
        if (*cp == '+') { cp++; v += term(ok); }
        else if (*cp == '-') { cp++; v -= term(ok); }
        else break;
    }
    return v;
}
/* 文件命中计数（虚拟文件集，UTF-8 子串匹配即可） */
static const char *const SPOT_FILES[] = {
    "项目计划书.pdf", "会议纪要.md", "壁纸.jpg", "README.txt", "预算表.numbers"
};
int mac_spotlight_search(const char *q, int *app_hit_mask, int *file_hit_count,
                         int *calc_valid, int *calc_val) {
    *app_hit_mask = 0; *file_hit_count = 0; *calc_valid = 0; *calc_val = 0;
    if (!q || !*q) return 0;
    for (int i = 0; i < MA_N; i++) {
        if (mac_ihas(mac_defs[i].cn, q) || mac_ihas(mac_defs[i].en, q))
            *app_hit_mask |= (1 << i);
    }
    for (int i = 0; i < 5; i++) if (mac_ihas(SPOT_FILES[i], q)) (*file_hit_count)++;
    /* 仅当表达式只含数字、空格与四则运算符及括号，且含运算符时才当作计算 */
    int has_op = 0, valid_chars = 1;
    for (const char *p = q; *p; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') continue;
        if (c == ' ' || c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')') {
            if (c=='+'||c=='-'||c=='*'||c=='/') has_op = 1;
            continue;
        }
        valid_chars = 0; break;
    }
    if (valid_chars && has_op) {
        cp = q; int ok = 1;
        int v = expr(&ok);
        while (*cp == ' ') cp++;
        if (ok && *cp == 0) { *calc_valid = 1; *calc_val = v; }
    }
    return (*app_hit_mask != 0) || (*file_hit_count > 0) || *calc_valid;
}
