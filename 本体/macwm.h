#ifndef MACWM_H
#define MACWM_H

/* ============================================================================
 * macwm.h —— macOS 桌面交互内核（纯逻辑 / 无硬件依赖 / 宿主与内核共用）
 * ----------------------------------------------------------------------------
 * 交互语义严格对齐 MacDesktop复刻/js/wm_core.js（64 条宿主单测已验证）。
 * 设计要点：
 *   1) App 与窗口完全解耦：一 App 拥有 0..N 窗口；关窗只毁视图，App 仍后台；
 *      只有 quit 才结束进程。
 *   2) 全局焦点是 active_app（非 active_window）；桌面属于 Finder，无"无焦点"态。
 *   3) 全部状态在 struct mac_state 内，固定数组、无动态分配，BSS 可控（<1KB）。
 *   4) 菜单/App 定义在 rodata（const），不占 BSS。
 * 本文件只用标准 C 类型（unsigned char/short/int），宿主 gcc 与 aarch64 gcc 通用。
 * ==========================================================================*/

#define MAC_NWIN    12      /* 桌面同时最多 12 个窗口 */
#define MAC_NSPACE  8       /* 虚拟桌面（含 1 个桌面 + 最多 7 个全屏 Space） */

/* App 编号：稳定，对应 wm_core.js 的 APP 常量；追加新 App 只能放末尾 */
enum mac_app {
    MA_FINDER = 0, MA_LAUNCHPAD, MA_SAFARI, MA_NOTES, MA_TERMINAL,
    MA_CALC, MA_PHOTOS, MA_MUSIC, MA_SETTINGS, MA_MAIL, MA_N
};

/* 窗口状态 */
enum mac_wstate { MW_NORMAL = 1, MW_MIN = 2, MW_FULL = 3 };
/* Space 类型 */
enum mac_space_type { MST_DESKTOP = 0, MST_FULL = 1 };
/* 菜单栏段 */
enum mac_menuseq { MMS_APPLE = 1, MMS_APP = 2, MMS_STATUS = 3 };

/* 语义事件（dispatch 的 type 参数） */
enum mac_evt {
    ME_LAUNCH = 1, ME_QUIT, ME_ACTIVATE, ME_HIDE, ME_NEWWIN,
    ME_FOCUSWIN, ME_CLOSEWIN, ME_MINWIN, ME_RESTOREWIN,
    ME_FULLSCREEN, ME_ZOOM, ME_DOCKCLICK, ME_DOCKRESTORE,
    ME_DOCKADD, ME_DOCKREMOVE, ME_CMDTAB, ME_CLICKDESKTOP,
    ME_MENUOPEN, ME_MENUCLICK, ME_SPOTLIGHT_TOGGLE, ME_LAUNCHPAD,
    ME_SPACE, ME_FORCEQUIT
};

/* 菜单动作（ME_MENUCLICK 的 a 参数） */
enum mac_action {
    MACT_ABOUT = 1, MACT_SETTINGS, MACT_RECENT, MACT_FORCEQUIT,
    MACT_SLEEP, MACT_RESTART, MACT_SHUTDOWN, MACT_LOCK,
    MACT_NEWWINDOW, MACT_CLOSEWINDOW, MACT_MINIMIZE, MACT_FULLSCREEN,
    MACT_HIDE, MACT_QUIT, MACT_FRONTALL, MACT_FOCUSWIN,
    MACT_SPOTLIGHT, MACT_LAUNCHPAD, MACT_GENERIC
};

struct mac_bounds { short x, y, w, h; };

struct mac_appst {
    unsigned short winmask;    /* 打开的窗口位图：bit i = wins[i] 属于本 App */
    unsigned short pid;        /* 0 = 未运行 */
    unsigned short launch_ts;
    unsigned char  running, hidden;
};

struct mac_win {
    struct mac_bounds b, restore;
    unsigned char used, app, state, space, hidden_by_app, z;
};

struct mac_space {
    unsigned short winmask;
    unsigned char used, type, app;
};

struct mac_state {
    struct mac_appst apps[MA_N];
    struct mac_win   wins[MAC_NWIN];
    struct mac_space spaces[MAC_NSPACE];
    unsigned char mru[MA_N], mru_n;      /* 最近使用 App 顺序 */
    unsigned short dock_mask;            /* bit i = App i 固定在 Dock */
    unsigned char active_app;            /* ★ 菜单栏中间段绑定这个 */
    short         focused_win;           /* -1 = 无焦点窗口 */
    short         cur_space;             /* 当前 space 下标 */
    unsigned short seq_pid, seq_win, seq_space;
    unsigned char ztop;
    unsigned char menu_open, menu_which, menu_app;
    unsigned char spotlight_open, launchpad_open, forcequit_open;
    unsigned short t;
    unsigned char last_evt;
};

/* App 静态定义（rodata） */
struct mac_appdef {
    const char *cn, *en;
    const char *const *menus;
    unsigned char nmenu, persistent, no_window, dock_zone; /* dock_zone: 1=left 2=mid */
};
extern const struct mac_appdef mac_defs[MA_N];

/* ---------------- API ---------------- */
void mac_init(struct mac_state *st);
void mac_dispatch(struct mac_state *st, int evt, int a, int b);

/* 派生查询（供渲染层 / 单测） */
const char *mac_app_cn(int app);
const char *mac_app_en(int app);
int  mac_app_running(struct mac_state *st, int app);
int  mac_app_hidden(struct mac_state *st, int app);
int  mac_app_pid(struct mac_state *st, int app);
int  mac_active(struct mac_state *st);
int  mac_focused(struct mac_state *st);
int  mac_win_count(struct mac_state *st, int app);
int  mac_win_state(struct mac_state *st, int win);
int  mac_win_app(struct mac_state *st, int win);
struct mac_bounds mac_win_bounds(struct mac_state *st, int win);
int  mac_menu_for_active(struct mac_state *st, const char **out, int cap);
int  mac_window_menu_wins(struct mac_state *st, int *out, int cap);
int  mac_dock_has(struct mac_state *st, int app);
int  mac_dock_zone(struct mac_state *st, int app);
int  mac_minimized_wins(struct mac_state *st, int *out, int cap);
/* Spotlight：返回搜索命中；calc_valid/calc_val 输出四则计算结果 */
int  mac_spotlight_search(const char *q, int *app_hit_mask,
                          int *file_hit_count, int *calc_valid, int *calc_val);

#endif /* MACWM_H */
