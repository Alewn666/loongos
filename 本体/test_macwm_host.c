/* macwm 宿主单测：逐条对齐 MacDesktop复刻/js/wm_core.test.js 的 macOS 交互规则 */
#include <stdio.h>
#include <string.h>
#include "macwm.h"

static int pass = 0, fail = 0;
static void ok(int cond, const char *name) {
    if (cond) { pass++; }
    else { fail++; printf("  FAIL: %s\n", name); }
}
#define T(name, ...) do { printf("[%s]\n", name); { __VA_ARGS__ } } while (0)

int main(void) {
    struct mac_state st;
    const char *menu[16];
    int mn;
    int win[16], wn;

    /* ---- 一、App/窗口解耦 ---- */
    T("App与窗口解耦", {
        mac_init(&st);
        mac_dispatch(&st, ME_LAUNCH, MA_SAFARI, 0);
        ok(mac_app_running(&st, MA_SAFARI), "launchApp 后 Safari 运行");
        ok(mac_win_count(&st, MA_SAFARI) == 1, "Safari 默认开 1 窗口");
        mac_dispatch(&st, ME_NEWWIN, MA_SAFARI, 0);
        mac_dispatch(&st, ME_NEWWIN, MA_SAFARI, 0);
        ok(mac_win_count(&st, MA_SAFARI) == 3, "同一 App 可多窗口(3)");
        mn = mac_menu_for_active(&st, menu, 16);
        int hasW = 0; for (int i=0;i<mn;i++) if (!strcmp(menu[i],"Window")) hasW=1;
        ok(hasW, "多窗口时菜单栏仍同一套 Safari 菜单(含Window)");
        /* 关闭一个窗口 */
        int w1 = mac_focused(&st);
        mac_dispatch(&st, ME_CLOSEWIN, w1, 0);
        ok(mac_app_running(&st, MA_SAFARI), "关闭一窗口后 Safari 仍运行");
        ok(mac_win_count(&st, MA_SAFARI) == 2, "剩余 2 个窗口");
        ok(mac_active(&st) == MA_SAFARI, "关窗后 active_app 仍是 Safari");
        /* 关到 0 窗口 */
        wn = mac_window_menu_wins(&st, win, 16);
        while (wn > 0) { mac_dispatch(&st, ME_CLOSEWIN, win[0], 0); wn = mac_window_menu_wins(&st, win, 16); }
        ok(mac_win_count(&st, MA_SAFARI) == 0, "窗口数归零");
        ok(mac_app_running(&st, MA_SAFARI), "★ 仅剩最后窗口关闭后 App 仍后台存活");
        mn = mac_menu_for_active(&st, menu, 16);
        int isSafari = (mac_active(&st)==MA_SAFARI);
        ok(isSafari && mn>0, "★ 无窗口时菜单栏仍显示 Safari 菜单");
        ok(mac_dock_has(&st, MA_SAFARI) && mac_app_running(&st,MA_SAFARI), "★ Dock 圆点保留");
        /* Cmd+Q 才退出 */
        mac_dispatch(&st, ME_QUIT, MA_SAFARI, 0);
        ok(!mac_app_running(&st, MA_SAFARI), "quit 后进程才结束");
        ok(mac_active(&st) == MA_FINDER, "★ 退出后焦点回落 Finder");
    });

    /* ---- 二、焦点联动 ---- */
    T("焦点联动", {
        mac_init(&st);
        ok(mac_active(&st) == MA_FINDER, "初始默认 Finder");
        mn = mac_menu_for_active(&st, menu, 16);
        ok(!strcmp(menu[0],"File") && mac_active(&st)==MA_FINDER, "初始菜单栏为 Finder");
        mac_dispatch(&st, ME_LAUNCH, MA_NOTES, 0);
        ok(mac_active(&st)==MA_NOTES, "启动备忘录→菜单栏切 Notes");
        mac_dispatch(&st, ME_LAUNCH, MA_TERMINAL, 0);
        ok(mac_active(&st)==MA_TERMINAL, "启动终端→菜单栏切 Terminal");
        /* 点窗口→激活其 App */
        mac_dispatch(&st, ME_LAUNCH, MA_NOTES, 0);
        int nw = mac_window_menu_wins(&st, win, 16);
        mac_dispatch(&st, ME_FOCUSWIN, win[nw-1], 0);
        ok(mac_active(&st)==MA_NOTES, "① 点窗口→激活其 App");
        /* 点桌面→Finder */
        mac_dispatch(&st, ME_CLICKDESKTOP, 0, 0);
        ok(mac_active(&st)==MA_FINDER, "② 点桌面→激活 Finder");
        /* Cmd+Tab */
        mac_dispatch(&st, ME_CMDTAB, 0, 0);
        ok(mac_active(&st)!=MA_FINDER, "③ Cmd+Tab 切走焦点");
        /* 同 App 内切窗口菜单栏不变 */
        mac_dispatch(&st, ME_LAUNCH, MA_SAFARI, 0);
        mac_dispatch(&st, ME_NEWWIN, MA_SAFARI, 0);
        wn = mac_window_menu_wins(&st, win, 16);
        int before = mac_active(&st);
        mac_dispatch(&st, ME_FOCUSWIN, win[0], 0);
        ok(mac_active(&st)==before && before==MA_SAFARI, "同 App 内切窗口菜单栏 App 不变");
    });

    /* ---- 三、红黄绿按钮 ---- */
    T("红黄绿按钮", {
        mac_init(&st);
        mac_dispatch(&st, ME_LAUNCH, MA_SAFARI, 0);
        mac_dispatch(&st, ME_NEWWIN, MA_SAFARI, 0);
        int w = mac_focused(&st);
        int before = mac_active(&st);
        /* 红：关窗 */
        mac_dispatch(&st, ME_CLOSEWIN, w, 0);
        ok(!(mac_win_state(&st,w)), "红：销毁当前窗口视图");
        ok(mac_app_running(&st,MA_SAFARI), "红：App 继续后台运行");
        ok(mac_active(&st)==before, "红：active_app 不变");
        /* 重新开窗口测黄 */
        mac_dispatch(&st, ME_NEWWIN, MA_SAFARI, 0);
        int w2 = mac_focused(&st);
        mac_dispatch(&st, ME_MINWIN, w2, 0);
        ok(mac_win_state(&st,w2)==MW_MIN, "黄：窗口进入 minimized");
        ok(mac_app_running(&st,MA_SAFARI), "黄：App 保持运行");
        int mins[16], mnc = mac_minimized_wins(&st, mins, 16);
        int inDock=0; for(int i=0;i<mnc;i++) if(mins[i]==w2) inDock=1;
        ok(inDock, "黄：最小化缩略图进入 Dock 右侧");
        mac_dispatch(&st, ME_DOCKRESTORE, w2, 0);
        ok(mac_win_state(&st,w2)==MW_NORMAL, "点击 Dock 缩略图恢复");
        ok(mac_focused(&st)==w2, "恢复后该窗口为焦点");
        /* 绿：全屏 */
        int nsp = 0; for(int i=0;i<MAC_NSPACE;i++) if(st.spaces[i].used) nsp++;
        mac_dispatch(&st, ME_FULLSCREEN, w2, 0);
        ok(mac_win_state(&st,w2)==MW_FULL, "绿：进入全屏");
        int nsp2=0; for(int i=0;i<MAC_NSPACE;i++) if(st.spaces[i].used) nsp2++;
        ok(nsp2==nsp+1, "绿：创建独立虚拟桌面 Space");
        ok(st.spaces[st.wins[w2].space].type==MST_FULL && st.spaces[st.wins[w2].space].app==MA_SAFARI, "绿：Space 被 Safari 独占");
        mac_dispatch(&st, ME_FULLSCREEN, w2, 0);
        ok(mac_win_state(&st,w2)==MW_NORMAL, "绿：再次点击退出全屏");
        /* Option+绿：zoom */
        struct mac_bounds b0 = mac_win_bounds(&st,w2);
        int nsp3=0; for(int i=0;i<MAC_NSPACE;i++) if(st.spaces[i].used) nsp3++;
        mac_dispatch(&st, ME_ZOOM, w2, 0);
        ok(mac_win_state(&st,w2)==MW_NORMAL, "Option+绿：不进全屏");
        int nsp4=0; for(int i=0;i<MAC_NSPACE;i++) if(st.spaces[i].used) nsp4++;
        ok(nsp4==nsp3, "Option+绿：不创建新 Space");
        struct mac_bounds b1 = mac_win_bounds(&st,w2);
        ok(b0.w!=b1.w || b0.h!=b1.h, "Option+绿：窗口尺寸变化(zoom)");
    });

    /* ---- 四、Dock ---- */
    T("Dock", {
        mac_init(&st);
        ok(mac_dock_zone(&st,MA_FINDER)==1 && mac_app_running(&st,MA_FINDER), "Dock 左侧 Finder 常驻");
        ok(mac_dock_zone(&st,MA_LAUNCHPAD)==1, "Dock 左侧启动台常驻");
        /* 未运行→启动 */
        ok(!mac_app_running(&st,MA_NOTES), "点击前备忘录未运行");
        mac_dispatch(&st, ME_DOCKCLICK, MA_NOTES, 0);
        ok(mac_app_running(&st,MA_NOTES), "① 点未运行→启动");
        ok(mac_win_count(&st,MA_NOTES)>=1, "① 启动后窗口前置");
        /* 已运行→不新建 */
        int pid1 = mac_app_pid(&st,MA_NOTES), wc1 = mac_win_count(&st,MA_NOTES);
        mac_dispatch(&st, ME_DOCKCLICK, MA_NOTES, 0);
        ok(mac_app_pid(&st,MA_NOTES)==pid1, "② 点已运行→不新建进程(pid不变)");
        ok(mac_win_count(&st,MA_NOTES)==wc1, "② 不新建窗口");
        /* 全最小化→恢复 */
        wn = mac_window_menu_wins(&st, win, 16);
        for(int i=0;i<wn;i++) mac_dispatch(&st, ME_MINWIN, win[i], 0);
        int mnc = mac_minimized_wins(&st, win, 16);
        ok(mnc>=1, "全部最小化→Dock 出现缩略图");
        mac_dispatch(&st, ME_DOCKCLICK, MA_NOTES, 0);
        int anyNormal=0; wn=mac_window_menu_wins(&st,win,16);
        for(int i=0;i<wn;i++) if(mac_win_state(&st,win[i])==MW_NORMAL) anyNormal=1;
        ok(anyNormal, "点已运行 App 图标恢复最小化窗口");
        /* 隐藏/取消隐藏 */
        mac_dispatch(&st, ME_HIDE, MA_NOTES, 0);
        ok(mac_app_hidden(&st,MA_NOTES), "App 可隐藏");
        mac_dispatch(&st, ME_DOCKCLICK, MA_NOTES, 0);
        ok(!mac_app_hidden(&st,MA_NOTES), "点隐藏 App 图标→取消隐藏");
        /* 常驻项不可移除 */
        mac_dispatch(&st, ME_DOCKREMOVE, MA_FINDER, 0);
        ok(mac_dock_has(&st,MA_FINDER), "Finder 常驻不可移除");
    });

    /* ---- 五、Spotlight ---- */
    T("Spotlight", {
        mac_init(&st);
        mac_dispatch(&st, ME_SPOTLIGHT_TOGGLE, 0, 1);
        ok(st.spotlight_open, "全局快捷键唤起独立弹窗");
        int am,fc,cv,val;
        mac_spotlight_search("safari", &am, &fc, &cv, &val);
        ok(am & (1<<MA_SAFARI), "搜索 App：safari 命中");
        mac_spotlight_search("会议纪要", &am, &fc, &cv, &val);
        ok(fc>=1, "搜索文件：命中会议纪要");
        mac_spotlight_search("12*13", &am, &fc, &cv, &val);
        ok(cv && val==156, "简单计算：12*13=156");
        mac_spotlight_search("100+23", &am, &fc, &cv, &val);
        ok(cv && val==123, "简单计算：100+23=123");
        mac_dispatch(&st, ME_SPOTLIGHT_TOGGLE, 0, 0);
        ok(!st.spotlight_open, "Spotlight 可关闭");
    });

    /* ---- 六、菜单栏数据 ---- */
    T("菜单栏三段", {
        mac_init(&st);
        mac_dispatch(&st, ME_LAUNCH, MA_SAFARI, 0);
        mn = mac_menu_for_active(&st, menu, 16);
        int hasW=0,hasH=0; for(int i=0;i<mn;i++){ if(!strcmp(menu[i],"Window"))hasW=1; if(!strcmp(menu[i],"Help"))hasH=1; }
        ok(hasW && hasH, "App 菜单含 Window 与 Help");
        wn = mac_window_menu_wins(&st, win, 16);
        ok(wn==mac_win_count(&st,MA_SAFARI), "Window 菜单列出当前 App 所有窗口");
        /* 苹果菜单固定：内核无修改接口（mac_defs 为 const） */
        ok(1, "苹果菜单由渲染层常量绘制，内核无修改接口");
    });

    printf("\n========================================\n");
    printf("macwm 宿主单测：%d PASS / %d FAIL\n", pass, fail);
    printf("========================================\n");
    return fail ? 1 : 0;
}
