/* ============================================================================
 * macdesktop.c —— LoongOS 上的 macOS 风格桌面外壳（渲染 + 输入适配）
 * ----------------------------------------------------------------------------
 * 逻辑内核见 macwm.c/.h（纯逻辑、宿主单测通过）。本文件只做两件事：
 *   1) 把 mac_state 用 gfx 原语画成高质感 macOS 桌面：
 *      渐变光晕壁纸 / 毛玻璃菜单栏 / 彩色 Dock / 圆角阴影窗口与真实内容；
 *   2) 把键鼠输入翻译成 mac_dispatch 事件，并支持拖动/悬停/置顶等手感。
 * 独立入口 macdesktop_run()，由 `macdesktop` 命令启动，不碰原 desktop.c。
 * ==========================================================================*/
#include "loongos.h"
#include "gfx_draw.h"
#include "macwm.h"
#include "rtc.h"

extern void gfxterm_stop(void);   /* 退出桌面时关闭键盘捕获（gfxterm.h） */

/* ---- 配色（0xRRGGBB） ---- */
#define C_WALL_TOP   0x1b3a6b   /* 壁纸顶：深蓝 */
#define C_WALL_BOT   0x0d1b33   /* 壁纸底：藏蓝 */
#define C_GLOW1      0x2e6fd0   /* 光晕蓝 */
#define C_GLOW2      0x7a4fd0   /* 光晕紫 */
#define C_GLOW3      0x1f9e9e   /* 光晕青 */
#define C_MBAR_BG    0xF6F6F7   /* 菜单栏毛玻璃基色 */
#define C_MBAR_FG    0x1d1d1f
#define C_MBAR_DIM   0x6e6e73
#define C_TITLE_A    0xF3F3F5   /* 标题栏渐变顶 */
#define C_TITLE_B    0xDCDCE0   /* 标题栏渐变底 */
#define C_TITLE_INA  0xE3E3E6   /* 非焦点标题 */
#define C_CONTENT    0xFFFFFF
#define C_SIDEBAR    0xE9E9EC
#define C_TEXT       0x1d1d1f
#define C_TEXT2      0x6e6e73
#define C_BORDER     0x86868B
#define C_SHADOW     0x000000
#define C_RED        0xFF5F57
#define C_YELLOW     0xFEBC2E
#define C_GREEN      0x28C840
#define C_ACCENT     0x007AFF
#define C_WHITE      0xFFFFFF
#define C_DOCKBG     0xD8D8DC
#define C_DOT        0x3a3a3c
#define C_PANEL      0xF9F9FA

/* ---- 布局 ---- */
#define MBAR_H     24
#define DOCK_H     62
#define TITLE_H    26
#define DOCK_ICON  40
#define DOCK_GAP   5
#define WINTITLE_R 9           /* 窗口圆角半径 */

static struct mac_state g_ms;
static int g_inited;
static int g_mx, g_my;
static int g_menu_drawn, g_menu_kind;
static int g_drag_win = -1;    /* 正在拖动的窗口下标，-1=无 */
static int g_drag_dx, g_drag_dy;/* 拖动锚点：鼠标-窗口左上 */
static int g_drag_ox, g_drag_oy;/* 上一帧窗口位置（局部重绘用） */
static int g_hover_dock = -1;  /* 当前鼠标悬停的 Dock App，-1=无 */
static int g_goto_classic = 0; /* 苹果菜单点 Classic Desktop → 1，退出后由调用方进原桌面 */
/* 光标脏区：箭头位图 20x19 + 右下投影 2px。移动时登记旧/新两块，擦旧画新。 */
#define CURSOR_BOX_W 22
#define CURSOR_BOX_H 21

/* 苹果菜单项数（含末尾 Classic Desktop 切换项） */
#define APPLE_MENU_N 9
#define APPLE_ITEM_CLASSIC 8   /* Classic Desktop 在菜单中的下标 */

/* 把矩形外扩 pad 后登记脏区（含阴影，拖动/悬停局部重绘用） */
static void damage_pad(struct gfx_rect r, int pad) {
    r.x-=pad; r.y-=pad; r.w+=2*pad; r.h+=2*pad;
    gfx_damage(r);
}

/* 当前下拉菜单矩形（与 draw_overlays / 命中测试共用同一坐标） */
static struct gfx_rect menu_rect(void) {
    int mx = (g_menu_kind==1)?8:(g_menu_kind==3)?320:150;
    int mh = (g_menu_kind==1) ? (APPLE_MENU_N*26+8) : 224;
    return (struct gfx_rect){mx,MBAR_H,240,mh};
}

/* 智能脏区：只登记「菜单栏 + Dock + 所有可见窗口」，跳过中间大片纯壁纸。
   点击/焦点变化/开关窗口时调用。gfx_present 会把每块脏区 clip 后整场景重画，
   所以壁纸 blend、窗口、菜单栏在脏区内都正确重绘，却省掉整屏 48 万像素混合。 */
static void damage_ui(void) {
    int W=gfx_width(), H=gfx_height();
    gfx_damage((struct gfx_rect){0,0,W,MBAR_H+2});
    gfx_damage((struct gfx_rect){0,H-DOCK_H-12,W,DOCK_H+12});
    for (int i=0;i<MAC_NWIN;i++) {
        struct mac_win *w=&g_ms.wins[i];
        if(!w->used || w->hidden_by_app) continue;
        if(w->state!=MW_NORMAL && w->state!=MW_FULL) continue; /* 最小化窗在 Dock 里，已覆盖 */
        struct gfx_rect r={w->b.x,w->b.y,w->b.w,w->b.h};
        if(w->state==MW_FULL){r.x=0;r.y=MBAR_H;r.w=W;r.h=H-MBAR_H;}
        damage_pad(r,10);
    }
}

/* 渲染层字符串等值比较与中文菜单映射。
 * 逻辑层 macwm.c 的菜单表保持英文（宿主单测 57 项依赖原文），
 * 只在帧缓冲渲染与菜单栏命中测试处统一转中文，保证两处宽度计算一致。 */
static int zeq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static const char *zh_menu(const char *s) {
    static const char *const EN[] = {
        "File","Edit","View","Go","Window","Help","History","Bookmarks",
        "Format","Shell","Controls","About This Mac","System Settings...",
        "Recent Items","Force Quit...","Sleep","Restart...","Shut Down...",
        "Lock Screen","Classic Desktop","Minimize","Bring All to Front",
        "Full Screen"
    };
    static const char *const ZH[] = {
        "文件","编辑","显示","前往","窗口","帮助","历史","书签",
        "格式","终端","控制","关于本机","系统设置",
        "最近使用的项目","强制退出","睡眠","重新启动","关机",
        "锁定屏幕","经典桌面","最小化","全部置于顶层",
        "全屏"
    };
    int n = (int)(sizeof(EN)/sizeof(EN[0]));
    for (int i = 0; i < n; i++) if (zeq(s, EN[i])) return ZH[i];
    return s;
}

/* App 主题色 / 图标底色 */
static u32 app_tint(int app) {
    switch (app) {
        case MA_FINDER:    return 0x1E9CFF;   /* 蓝白笑脸 */
        case MA_LAUNCHPAD: return 0xB0B0B8;   /* 灰 */
        case MA_SAFARI:    return 0x16A9FF;
        case MA_NOTES:     return 0xFFCC00;
        case MA_TERMINAL:  return 0x232326;
        case MA_CALC:      return 0x4A4A4E;
        case MA_PHOTOS:    return 0xF5F5F7;
        case MA_MUSIC:     return 0xFA2D48;
        case MA_SETTINGS:  return 0x8E8E93;
        case MA_MAIL:      return 0x1479FF;
        default:           return 0x1E9CFF;
    }
}

static struct gfx_rect full(void) { struct gfx_rect r = {0,0,gfx_width(),gfx_height()}; return r; }
static int hit(struct gfx_rect r, int x, int y) {
    return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;
}
static int mac_isqrt(int v) { int r=0; while((r+1)*(r+1)<=v) r++; return r; }

/* =========================================================================
 * 壁纸：深蓝垂直渐变 + 三团柔和光晕（同心半透明圆，无需位图）
 * ========================================================================= */
static void draw_wallpaper(void) {
    int W = gfx_width(), H = gfx_height();
    gfx_vgrad((struct gfx_rect){0,0,W,H}, C_WALL_TOP, C_WALL_BOT);
    /* 柔和径向光晕：边缘二次衰减，无实心圆硬边。几团错落叠出层次。 */
    gfx_soft_circle(W*72/100, H*26/100, H*2/5, C_GLOW2, 120); /* 紫 */
    gfx_soft_circle(W*72/100, H*26/100, H*3/10, C_GLOW1, 110); /* 蓝 */
    gfx_soft_circle(W*14/100, H*74/100, H*2/5, C_GLOW1, 100);  /* 左下蓝 */
    gfx_soft_circle(W*10/100, H*82/100, H*1/4, C_GLOW3, 90);   /* 青 */
}

/* 画苹果符号：用圆+缺口近似（纯几何，不依赖字体） */
static void draw_apple(int x, int y, u32 c) {
    /* 身体：两个交叠圆 + 下半矩形都在一个实心圆里近似 */
    gfx_filled_circle(x+6, y+9, 6, c);
    gfx_filled_circle(x+12, y+9, 6, c);
    gfx_fill((struct gfx_rect){x+1, y+9, 16, 8}, c);
    /* 叶子：右上角小圆 */
    gfx_filled_circle(x+13, y+1, 3, c);
    /* 咬痕：右中用壁纸附近色挖掉——用透明做不到，用深色小圆近似 */
    gfx_filled_circle(x+16, y+10, 2, C_MBAR_BG);
}

/* 顶部状态图标（纯几何）；从右边缘反向布局，互不重叠 */
static void draw_status_icons(int W) {
    int y = 6;
    /* 聚光放大镜（最右） */
    gfx_rect_draw((struct gfx_rect){W-20,y-1,8,8}, C_MBAR_FG, 1);
    gfx_line(W-12, y+7, W-9, y+10, C_MBAR_FG);
    /* Wi-Fi 扇形（阶梯近似） */
    int wx = W - 44;
    gfx_fill((struct gfx_rect){wx+3,y+2,6,2}, C_MBAR_FG);
    gfx_fill((struct gfx_rect){wx+1,y+4,10,2}, C_MBAR_FG);
    gfx_fill((struct gfx_rect){wx,y+6,12,2}, C_MBAR_FG);
    /* 电池 */
    int bx = W - 76;
    gfx_rect_draw((struct gfx_rect){bx,y+2,20,9}, C_MBAR_FG, 1);
    gfx_fill((struct gfx_rect){bx+1,y+3,13,7}, C_MBAR_FG);
    gfx_fill((struct gfx_rect){bx+20,y+5,2,4}, C_MBAR_FG);
    /* 时间：读 PL031 真实墙钟，中文格式「周三 14:07」。
       RTC 不可用时回退「周三 --:--」，布局宽度不变。 */
    char tbuf[16];
    u64 ep = rtc_now();
    int wd = -1, hh = -1, mi = -1;
    if (ep) {
        int yy,mo,dd,ss;
        rtc_breakdown(ep,&yy,&mo,&dd,&hh,&mi,&ss,&wd);
    }
    static const char *const wdname[7]={"周日","周一","周二","周三","周四","周五","周六"};
    const char *wn = (wd>=0&&wd<7)?wdname[wd]:"--";
    /* 时分 */
    if (hh>=0) {
        tbuf[0]='0'+((hh/10)%10); tbuf[1]='0'+(hh%10);
        tbuf[2]=':';
        tbuf[3]='0'+((mi/10)%10); tbuf[4]='0'+(mi%10);
        tbuf[5]=0;
    } else {
        tbuf[0]='-'; tbuf[1]='-'; tbuf[2]=':'; tbuf[3]='-'; tbuf[4]='-'; tbuf[5]=0;
    }
    int tw=gfx_text_w(tbuf), ww=gfx_text_w(wn);
    gfx_text(bx-8-tw, y, tbuf, C_MBAR_FG, 0xFFFFFFFF);
    gfx_text(bx-8-tw-ww-4, y, wn, C_MBAR_FG, 0xFFFFFFFF);
}

/* =========================================================================
 * 菜单栏：半透明毛玻璃 + 左苹果 + 当前 App 菜单 + 右状态
 * ========================================================================= */
static void draw_menubar(void) {
    int W = gfx_width();
    /* 毛玻璃：基色半透明覆盖壁纸 */
    gfx_blend((struct gfx_rect){0,0,W,MBAR_H}, C_MBAR_BG, 225);
    gfx_fill((struct gfx_rect){0,MBAR_H-1,W,1}, 0xC8C8CC); /* 底部分隔线 */

    draw_apple(8, 4, C_MBAR_FG);
    /* App 名（中文，粗感：画两遍错位 1px 模拟加粗） */
    const char *appname = mac_app_cn(g_ms.active_app);
    gfx_text(34, 5, appname, C_MBAR_FG, 0xFFFFFFFF);
    gfx_text(35, 5, appname, C_MBAR_FG, 0xFFFFFFFF);
    /* 当前 App 的菜单项（中文） */
    const char *menus[16];
    int n = mac_menu_for_active(&g_ms, menus, 16);
    int x = 34 + gfx_text_w(appname) + 18;
    for (int i = 0; i < n; i++) {
        const char *mzh = zh_menu(menus[i]);
        gfx_text(x, 5, mzh, (i==0)?C_MBAR_FG:C_MBAR_DIM, 0xFFFFFFFF);
        x += gfx_text_w(mzh) + 13;
    }
    draw_status_icons(W);
}

/* =========================================================================
 * 窗口：圆角阴影 + 立体标题栏 + 三色圆形按钮 + 按 App 的内容
 * ========================================================================= */

/* 画一个 App 图标（圆角底 + 几何内容），size 通常 28~40 */
static void draw_app_icon(int app, int x, int y, int s) {
    u32 tint = app_tint(app);
    int r = s/5;
    gfx_roundrect((struct gfx_rect){x,y,s,s}, tint, r);
    /* 顶部高光：上半部分浅一点 */
    gfx_blend((struct gfx_rect){x+r/2,y+r/2,s-r,s/3}, C_WHITE, 40);
    switch (app) {
        case MA_FINDER: {   /* 蓝底 + 白笑脸：两眼 + 弧形嘴 */
            gfx_fill((struct gfx_rect){x+s/3,y+s/2-2,3,5}, C_WHITE);
            gfx_fill((struct gfx_rect){x+2*s/3,y+s/2-2,3,5}, C_WHITE);
            gfx_filled_circle(x+s/2, y+2*s/3, s/6, C_WHITE);       /* 嘴 */
            gfx_fill((struct gfx_rect){x+s/2-s/6,y+2*s/3-2,s/3,s/6}, tint); /* 挖出上半 */
            gfx_fill((struct gfx_rect){x+s/2-3,y+4,6,2}, C_WHITE);  /* 刘海 */
            break;
        }
        case MA_SAFARI: {   /* 蓝底白圆 + 红指针 */
            gfx_filled_circle(x+s/2, y+s/2, s/2-5, 0xE8F4FF);
            gfx_filled_circle(x+s/2, y+s/2, 2, C_ACCENT);
            gfx_line(x+s/2-6, y+s/2+6, x+s/2+2, y+s/2-2, C_RED);
            gfx_line(x+s/2-5, y+s/2+5, x+s/2, y+s/2, C_RED);
            break;
        }
        case MA_MAIL: {     /* 蓝底白信封 */
            gfx_fill((struct gfx_rect){x+6,y+s/3,s-12,s/2}, C_WHITE);
            gfx_line(x+6,y+s/3,x+s/2,y+s/2+2,C_ACCENT);
            gfx_line(x+s-6,y+s/3,x+s/2,y+s/2+2,C_ACCENT);
            break;
        }
        case MA_MUSIC: {    /* 红底白音符 */
            gfx_filled_circle(x+s/2-4, y+s-9, 4, C_WHITE);
            gfx_filled_circle(x+s-10, y+s-11, 3, C_WHITE);
            gfx_fill((struct gfx_rect){x+s/2-1,y+8,3,s-17}, C_WHITE);
            gfx_fill((struct gfx_rect){x+s-7,y+8,3,s-19}, C_WHITE);
            gfx_fill((struct gfx_rect){x+s/2-1,y+8,s-8,3}, C_WHITE);
            break;
        }
        case MA_PHOTOS: {   /* 白底彩色花瓣 */
            gfx_filled_circle(x+s/2,y+s/2-5,4,0xFF3B30);
            gfx_filled_circle(x+s/2+6,y+s/2,4,0xFFCC00);
            gfx_filled_circle(x+s/2,y+s/2+6,4,0x34C759);
            gfx_filled_circle(x+s/2-6,y+s/2,4,0x007AFF);
            gfx_filled_circle(x+s/2,y+s/2,3,0xFF2D55);
            break;
        }
        case MA_NOTES: {    /* 黄底横线 */
            gfx_fill((struct gfx_rect){x,y,s,7},0xFFCC00);
            for (int i=0;i<3;i++) gfx_fill((struct gfx_rect){x+6,y+13+i*7,s-12,1},0xB59A00);
            break;
        }
        case MA_CALC: {     /* 深灰底橙等号 */
            gfx_fill((struct gfx_rect){x+9,y+s/3-2,s-18,3},0xFF9F0A);
            gfx_fill((struct gfx_rect){x+9,y+2*s/3-2,s-18,3},0xFF9F0A);
            break;
        }
        case MA_TERMINAL: { /* 黑底 >_ */
            gfx_text(x+6,y+s/2-8,">_",0x34C759,0xFFFFFFFF);
            break;
        }
        case MA_SETTINGS: { /* 灰底齿轮（同心圆+齿） */
            gfx_filled_circle(x+s/2,y+s/2,s/3,0x636366);
            gfx_filled_circle(x+s/2,y+s/2,s/6,0xD8D8DC);
            break;
        }
        case MA_LAUNCHPAD: {/* 灰底彩色小格 */
            for (int i=0;i<3;i++) for(int j=0;j<3;j++)
                gfx_fill((struct gfx_rect){x+7+i*9,y+7+j*9,6,6},
                         ((i+j)&1)?0xFF3B30:0x007AFF);
            break;
        }
        default: gfx_filled_circle(x+s/2,y+s/2,s/4,C_WHITE); break;
    }
}

/* Finder 内容：左侧边栏 + 工具栏 + 文件网格 */
static void content_finder(struct gfx_rect c) {
    gfx_fill(c, C_CONTENT);
    int sw = 118;   /* 侧边栏宽 */
    gfx_fill((struct gfx_rect){c.x,c.y,sw,c.h}, C_SIDEBAR);
    gfx_fill((struct gfx_rect){c.x+sw,c.y,1,c.h}, 0xD0D0D4);
    static const char *const side[] = {"最近使用","隔空投送","应用程序","桌面","文稿","下载","图片","云盘"};
    for (int i=0;i<8;i++) {
        int yy = c.y+30+i*22;
        gfx_fill((struct gfx_rect){c.x+10,yy+3,9,9}, (i&1)?0x007AFF:0x5B9BF3);
        gfx_text(c.x+26, yy, side[i], C_TEXT, 0xFFFFFFFF);
    }
    /* 工具栏 */
    gfx_fill((struct gfx_rect){c.x+sw,c.y,c.w-sw,34}, 0xF5F5F7);
    gfx_text(c.x+sw+14,c.y+9,"<   >   云盘",C_TEXT,0xFFFFFFFF);
    gfx_line(c.x+sw,c.y+34,c.x+c.w,c.y+34,0xD0D0D4);
    /* 文件网格：彩色文件夹 + 图片缩略 */
    static const char *const names[] = {"桌面","文稿","照片","音乐","影片","项目","自述","图标"};
    for (int i=0;i<8;i++) {
        int col=i%4, row=i/4;
        int fx = c.x+sw+24+col*90, fy = c.y+56+row*96;
        u32 fc = (i&1)?0x4FA3F7:0x6FC2F0;
        gfx_fill((struct gfx_rect){fx,fy,56,40}, fc);
        gfx_fill((struct gfx_rect){fx,fy-5,24,7}, fc);  /* 文件夹标签 */
        gfx_text(fx, fy+46, names[i], C_TEXT2, 0xFFFFFFFF);
    }
}

/* Safari 内容：标签栏 + 地址栏 + 网页占位 */
static void content_safari(struct gfx_rect c) {
    gfx_fill(c, 0xFFFFFF);
    gfx_fill((struct gfx_rect){c.x,c.y,c.w,26}, 0xE8E8EB);
    gfx_text(c.x+12,c.y+6,"龙 OS",C_TEXT,0xFFFFFFFF);
    gfx_fill((struct gfx_rect){c.x,c.y+26,c.w,30}, 0xF5F5F7);
    gfx_roundrect((struct gfx_rect){c.x+30,c.y+31,c.w-60,20}, 0xFFFFFF, 6);
    gfx_text(c.x+40,c.y+32,"https://loongos.local",C_TEXT2,0xFFFFFFFF);
    gfx_line(c.x,c.y+56,c.x+c.w,c.y+56,0xD8D8DC);
    /* 网页占位：标题 + 段落条 */
    gfx_text(c.x+40,c.y+80,"欢迎使用 LoongOS",0x1d1d1f,0xFFFFFFFF);
    for (int i=0;i<6;i++)
        gfx_fill((struct gfx_rect){c.x+40,c.y+108+i*20, (i%2)?c.w-120:c.w-80, 8}, 0xE0E0E4);
}

/* 终端内容：黑底 + 命令行 */
static void content_terminal(struct gfx_rect c) {
    gfx_fill(c, 0x1C1C1E);
    gfx_text(c.x+16,c.y+16,"loongos@mac ~ %",0x34C759,0x1C1C1E);
    gfx_text(c.x+16,c.y+38,"uname -a",0xF5F5F7,0x1C1C1E);
    gfx_text(c.x+16,c.y+60,"龙 OS 60.0 aarch64 内核",0x5AC8FA,0x1C1C1E);
    gfx_text(c.x+16,c.y+82,"loongos@mac ~ %",0x34C759,0x1C1C1E);
    gfx_fill((struct gfx_rect){c.x+130,c.y+82,8,16},0xF5F5F7); /* 光标 */
}

/* 计算器内容：显示区 + 按钮网格 */
static void content_calc(struct gfx_rect c) {
    gfx_fill(c, 0x1C1C1E);
    gfx_text(c.x+c.w-60,c.y+24,"0",0xFFFFFF,0x1C1C1E);
    static const char *const keys[] = {"AC","+/-","%","/","7","8","9","*","4","5","6","-","1","2","3","+"};
    int kw=44, kh=30, gx=8, gy=8;
    int bx=c.x+(c.w-(kw*4+gx*3))/2, by=c.y+60;
    for (int i=0;i<16;i++) {
        int col=i%4,row=i/4;
        u32 bg=(col==3)?0xFF9F0A:0x5A5A5E;
        gfx_roundrect((struct gfx_rect){bx+col*(kw+gx),by+row*(kh+gy),kw,kh}, bg, 6);
        gfx_text(bx+col*(kw+gx)+14, by+row*(kh+gy)+8, keys[i], 0xFFFFFF, 0xFFFFFFFF);
    }
}

/* 系统设置内容：左侧列表 + 右侧分组 */
static void content_settings(struct gfx_rect c) {
    gfx_fill(c, 0xFFFFFF);
    int sw=130;
    gfx_fill((struct gfx_rect){c.x,c.y,sw,c.h},0xF2F2F5);
    static const char *const rows[] = {"无线局域网","蓝牙","网络","声音","显示器","墙纸","电池","通用"};
    for(int i=0;i<8;i++){
        int yy=c.y+18+i*26;
        if(i==0) gfx_fill((struct gfx_rect){c.x,yy-4,sw,24},0xFF9F0A);
        gfx_fill((struct gfx_rect){c.x+10,yy,12,12},(i&1)?0x007AFF:0x8E8E93);
        gfx_text(c.x+28,yy-2,rows[i],(i==0)?C_WHITE:C_TEXT,0xFFFFFFFF);
    }
    gfx_text(c.x+sw+30,c.y+24,"无线局域网",0x1d1d1f,0xFFFFFFFF);
    gfx_roundrect((struct gfx_rect){c.x+sw+30,c.y+58,c.w-sw-60,44},0xF5F5F7,8);
    gfx_text(c.x+sw+44,c.y+68,"网络名称",C_TEXT,0xFFFFFFFF);
    gfx_text(c.x+sw+44,c.y+84,"已连接",C_TEXT2,0xFFFFFFFF);
    gfx_filled_circle(c.x+c.w-50,c.y+76,9,0x34C759);
}

/* 备忘录内容：黄标题 + 便签列表 */
static void content_notes(struct gfx_rect c) {
    gfx_fill(c,0xFFFFFF);
    int sw=120;
    gfx_fill((struct gfx_rect){c.x,c.y,sw,c.h},0xF7F7F2);
    for(int i=0;i<5;i++){
        gfx_roundrect((struct gfx_rect){c.x+8,c.y+10+i*44,sw-16,38},(i==0)?0xFFE08A:0xEDEDE8,4);
        gfx_text(c.x+16,c.y+16+i*44,"新备忘录",C_TEXT,0xFFFFFFFF);
        gfx_text(c.x+16,c.y+30+i*44,"龙 OS 使用说明...",C_TEXT2,0xFFFFFFFF);
    }
    gfx_fill((struct gfx_rect){c.x+sw,c.y,1,c.h},0xD0D0D4);
    gfx_text(c.x+sw+20,c.y+20,"备忘录",C_TEXT,0xFFFFFFFF);
    for(int i=0;i<5;i++) gfx_text(c.x+sw+20,c.y+50+i*24,"- 文本行内容",C_TEXT2,0xFFFFFFFF);
}

/* 照片/音乐/邮件通用网格占位 */
static void content_generic(struct gfx_rect c, int app) {
    gfx_fill(c, 0xFFFFFF);
    gfx_fill((struct gfx_rect){c.x,c.y,c.w,32},0xF5F5F7);
    gfx_text(c.x+14,c.y+9,mac_app_cn(app),C_TEXT,0xFFFFFFFF);
    u32 palette[] = {0xFF3B30,0xFF9500,0xFFCC00,0x34C759,0x007AFF,0x5856D6,0xFF2D55,0x5AC8FA};
    for(int i=0;i<12;i++){
        int col=i%4,row=i/4;
        int fx=c.x+16+col*((c.w-32)/4), fy=c.y+48+row*66;
        gfx_roundrect((struct gfx_rect){fx,fy,(c.w-32)/4-10,56},palette[i%8],6);
    }
}

static void draw_window_content(int app, struct gfx_rect c) {
    gfx_clip_push(c);
    switch(app) {
        case MA_FINDER:    content_finder(c); break;
        case MA_SAFARI:    content_safari(c); break;
        case MA_TERMINAL:  content_terminal(c); break;
        case MA_CALC:      content_calc(c); break;
        case MA_SETTINGS:  content_settings(c); break;
        case MA_NOTES:     content_notes(c); break;
        default:           content_generic(c, app); break;
    }
    gfx_clip_pop();
}

static void draw_windows(void) {
    int H = gfx_height();
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
        int focused = (pick == g_ms.focused_win);
        /* 阴影：底部和右侧偏移的黑色半透明圆角 */
        gfx_blend_roundrect((struct gfx_rect){r.x+4,r.y+6,r.w,r.h}, C_SHADOW, WINTITLE_R, 55);
        /* 窗口主体：白底圆角 */
        gfx_roundrect(r, C_CONTENT, WINTITLE_R);
        /* 标题栏渐变：顶部两行按圆角缩进，避免盖住白色圆角窗口的方角 */
        u32 ta = focused?C_TITLE_A:C_TITLE_INA;
        gfx_vgrad((struct gfx_rect){r.x+1,r.y+WINTITLE_R,r.w-2,TITLE_H-1-WINTITLE_R}, ta, C_TITLE_B);
        for (int yy=0; yy<WINTITLE_R; yy++) {
            int off = WINTITLE_R - mac_isqrt(WINTITLE_R*WINTITLE_R - (WINTITLE_R-1-yy)*(WINTITLE_R-1-yy));
            u32 c = (yy < WINTITLE_R/2) ? ta : C_TITLE_B;
            gfx_fill((struct gfx_rect){r.x+1+off, r.y+yy, r.w-2-2*off, 1}, c);
        }
        gfx_fill((struct gfx_rect){r.x+1,r.y+TITLE_H-1,r.w-2,1}, 0xC6C6CA);
        /* 三色圆按钮（实心圆 + 深色描边圈） */
        int by = r.y + TITLE_H/2;
        gfx_filled_circle(r.x+16, by, 6, C_RED);
        gfx_filled_circle(r.x+36, by, 6, C_YELLOW);
        gfx_filled_circle(r.x+56, by, 6, C_GREEN);
        gfx_rect_draw((struct gfx_rect){r.x+10,by-6,12,12}, 0xD0D0D4, 1);
        gfx_rect_draw((struct gfx_rect){r.x+30,by-6,12,12}, 0xD0D0D4, 1);
        gfx_rect_draw((struct gfx_rect){r.x+50,by-6,12,12}, 0xD0D0D4, 1);
        /* 标题文字（居中，中文 App 名） */
        const char *nm = mac_app_cn(w->app);
        int tw = gfx_text_w(nm);
        gfx_text(r.x+(r.w-tw)/2, r.y+6, nm, C_TEXT, 0xFFFFFFFF);
        /* 内容区 */
        struct gfx_rect cr = {r.x+1,r.y+TITLE_H,r.w-2,r.h-TITLE_H-1};
        draw_window_content(w->app, cr);
    }
}

/* =========================================================================
 * Dock：底部居中圆角毛玻璃 + 彩色图标 + 运行圆点 + 悬停放大
 * ========================================================================= */
static int dock_x0(void) {
    int W=gfx_width(), n=0, total=0;
    for(int a=0;a<MA_N;a++) if(mac_dock_has(&g_ms,a)) n++;
    int mins[16], mnc=mac_minimized_wins(&g_ms,mins,16);
    total = (n+mnc+1)*(DOCK_ICON+DOCK_GAP) - DOCK_GAP + 16; /* +1 废纸篓 */
    int x = (W-total)/2; if(x<4)x=4;
    return x;
}
static int dock_y0(void) { return gfx_height() - DOCK_H + 10; }

/* 遍历 Dock 得到每项 (x, app, win)，win>=0 表示最小化窗口缩略图 */
static int dock_items(int *xs, int *apps, int *wins, int cap) {
    int n=0, x=dock_x0(), y=dock_y0();
    (void)y;
    for(int a=0;a<MA_N;a++){
        if(n>=cap)break;
        if(mac_dock_has(&g_ms,a)){ xs[n]=x; apps[n]=a; wins[n]=-1; n++; x+=DOCK_ICON+DOCK_GAP; }
    }
    int mins[16], mnc=mac_minimized_wins(&g_ms,mins,16);
    for(int i=0;i<mnc && n<cap;i++){
        xs[n]=x; apps[n]=mac_win_app(&g_ms,mins[i]); wins[n]=mins[i]; n++; x+=DOCK_ICON+DOCK_GAP;
    }
    xs[n]=x+16; apps[n]=-1; wins[n]=-2; n++; /* 废纸篓：紧跟图标组，+16 分隔与 dock_x0() 的 total 计算一致 */
    return n;
}

static void draw_dock(void) {
    int H=gfx_height();
    int xs[32], apps[32], wins[32];
    int n = dock_items(xs,apps,wins,32);
    int x0 = xs[0]-10, x1 = xs[n-1]+DOCK_ICON+10;
    int dw = x1-x0, dy = H-DOCK_H+6;
    /* 毛玻璃底板 */
    gfx_blend_roundrect((struct gfx_rect){x0,dy,dw,DOCK_H-8}, C_DOCKBG, 14, 215);
    gfx_roundrect((struct gfx_rect){x0,dy,dw,2}, 0xFFFFFF, 1); /* 顶部高光 */
    for (int i=0;i<n;i++){
        /* App 区与废纸篓之间的分隔竖线（macOS 风格） */
        if (wins[i]==-2 && i>0) gfx_fill((struct gfx_rect){xs[i]-9, dock_y0()+2, 1, DOCK_ICON-4}, 0xB0B0B5);
        int x=xs[i], y=dock_y0(), s=DOCK_ICON;
        /* 悬停放大：仅 App 图标（排除废纸篓 wins=-2、最小化缩略 wins>=0），上移 + 变大 */
        int hover = (apps[i]>=0 && wins[i]==-1 && apps[i]==g_hover_dock);
        if(hover){ y-=8; s+=8; x-=4; }
        if(wins[i]==-2){ /* 废纸篓 */
            gfx_roundrect((struct gfx_rect){x,y,s,s},0x9A9AA0,8);
            gfx_fill((struct gfx_rect){x+s/2-7,y+8,14,3},0xE0E0E4);
            gfx_fill((struct gfx_rect){x+s/2-5,y+6,10,4},0xE0E0E4);
        } else if(wins[i]>=0){ /* 最小化窗口缩略 */
            gfx_roundrect((struct gfx_rect){x,y,s,s},app_tint(apps[i]),8);
            gfx_rect_draw((struct gfx_rect){x+4,y+8,s-8,s-16},C_WHITE,1);
        } else {
            draw_app_icon(apps[i], x, y, s);
            if(mac_app_running(&g_ms,apps[i])){
                gfx_filled_circle(x+s/2, y+s+5, 2, C_DOT); /* 运行圆点 */
            }
        }
    }
}

/* =========================================================================
 * 下拉菜单 / Spotlight 覆盖层
 * ========================================================================= */
static void draw_overlays(void) {
    if (g_ms.spotlight_open) {
        int W=gfx_width();
        gfx_blend_roundrect((struct gfx_rect){W/2-180,90,360,60}, C_PANEL, 12, 245);
        gfx_text(W/2-165,110,"聚焦搜索",C_TEXT2,0xFFFFFFFF);
        gfx_text(W/2-165,128,"键入以搜索应用...",C_TEXT,0xFFFFFFFF);
    }
    if (!g_menu_drawn) return;
    struct gfx_rect m = menu_rect();
    gfx_blend_roundrect(m, C_PANEL, 8, 250);
    gfx_rect_draw(m, C_BORDER, 1);
    if (g_menu_kind == 1) {
        static const char *const items[APPLE_MENU_N] = {"关于本机","系统设置","最近使用的项目","强制退出","睡眠","重新启动","关机","锁定屏幕","经典桌面"};
        for (int i=0;i<APPLE_MENU_N;i++) {
            if(i==3 || i==APPLE_ITEM_CLASSIC-1) gfx_fill((struct gfx_rect){m.x+4,m.y+8+i*26,m.w-8,1},0xD0D0D4);
            gfx_text(m.x+14, m.y+10+i*26, items[i], (i==APPLE_ITEM_CLASSIC)?C_ACCENT:C_TEXT, 0xFFFFFFFF);
        }
    } else if (g_menu_kind == 3) {
        int wins[16], n = mac_window_menu_wins(&g_ms, wins, 16);
        for (int i=0;i<n;i++) gfx_text(m.x+14, m.y+8+i*22, mac_app_cn(mac_win_app(&g_ms,wins[i])), C_TEXT, 0xFFFFFFFF);
        int y = m.y+8+n*22+6;
        gfx_fill((struct gfx_rect){m.x+4,y-4,m.w-8,1},0xD0D0D4);
        static const char *const acts[]={"最小化","全部置于顶层","全屏"};
        for(int i=0;i<3;i++) gfx_text(m.x+14,y+i*22,acts[i],C_TEXT,0xFFFFFFFF);
    } else {
        const char *menus[16];
        int n = mac_menu_for_active(&g_ms, menus, 16);
        for (int i=0;i<n;i++) gfx_text(m.x+14, m.y+10+i*26, zh_menu(menus[i]), C_TEXT, 0xFFFFFFFF);
    }
}

static void mac_redraw(struct gfx_rect r) {
    (void)r;
    draw_wallpaper();
    draw_windows();
    draw_menubar();
    draw_dock();
    draw_overlays();
    /* 光标永远画在最上层；每次脏区重绘都会重画，配合主循环的旧/新脏区登记，
       实现「移动到哪擦到哪、画到哪」，不留拖尾。 */
    gfx_cursor(g_mx, g_my);
}

/* =========================================================================
 * 命中测试
 * ========================================================================= */
static int dock_app_at(int x, int y) {
    int xs[32], apps[32], wins[32];
    int n=dock_items(xs,apps,wins,32);
    for (int i=0;i<n;i++){
        int s=DOCK_ICON, yy=dock_y0();
        if(apps[i]>=0 && wins[i]==-1 && apps[i]==g_hover_dock){ s+=8; yy-=8; }
        if(hit((struct gfx_rect){xs[i]-(s-DOCK_ICON)/2,yy,s,s}, x, y)) return apps[i];
    }
    return -1;
}
static int dock_win_at(int x, int y) {
    int xs[32], apps[32], wins[32];
    int n=dock_items(xs,apps,wins,32);
    for (int i=0;i<n;i++) if(wins[i]>=0)
        if(hit((struct gfx_rect){xs[i],dock_y0(),DOCK_ICON,DOCK_ICON},x,y)) return wins[i];
    return -1;
}
static int top_win_at(int x, int y) {
    int best=-1,bz=-1;
    for(int i=0;i<MAC_NWIN;i++){
        if(!g_ms.wins[i].used||g_ms.wins[i].hidden_by_app)continue;
        if(g_ms.wins[i].state!=MW_NORMAL&&g_ms.wins[i].state!=MW_FULL)continue;
        struct gfx_rect r={g_ms.wins[i].b.x,g_ms.wins[i].b.y,g_ms.wins[i].b.w,g_ms.wins[i].b.h};
        if(g_ms.wins[i].state==MW_FULL){r.x=0;r.y=MBAR_H;r.w=gfx_width();r.h=gfx_height()-MBAR_H;}
        if(!hit(r,x,y))continue;
        if(g_ms.wins[i].z>bz){bz=g_ms.wins[i].z;best=i;}
    }
    return best;
}
/* 标题栏可拖动区（排除三色按钮） */
static int on_titlebar(int wi, int x, int y) {
    struct mac_win *w=&g_ms.wins[wi];
    if(w->state!=MW_NORMAL)return 0;
    return (y>=w->b.y && y<w->b.y+TITLE_H && x>w->b.x+70);
}

/* 命中测试：菜单栏菜单项，返回菜单类型 */
static int menubar_hit(int x) {
    /* 苹果区 */
    if(x<28) return 1;
    const char *menus[16];
    int n=mac_menu_for_active(&g_ms,menus,16);
    int x0=34+gfx_text_w(mac_app_cn(g_ms.active_app))+18;
    int x1=x0;
    for(int i=0;i<n;i++) x1+=gfx_text_w(zh_menu(menus[i]))+13;
    if(x>=x0 && x<x1) return 2;
    if(x>=320 && x<560) return 3;
    return 0;
}

static void on_press(int x, int y) {
    /* 下拉菜单优先吃点击 */
    if (g_menu_drawn) {
        struct gfx_rect oldmenu = menu_rect();  /* 关闭前先记录，供擦除 */
        if (g_menu_kind == 1) {
            /* 苹果菜单：按 y 命中各项；最后一项 Classic Desktop → 切到原桌面 */
            struct gfx_rect m={8,MBAR_H,240,APPLE_MENU_N*26+8};
            if(hit(m,x,y)){
                int idx=(y-(m.y+8))/26;
                if(idx==APPLE_ITEM_CLASSIC){ g_goto_classic=1; }
            }
        } else if (g_menu_kind == 3) {
            struct gfx_rect m={320,MBAR_H,240,224};
            if(hit(m,x,y)){
                int wins[16],n=mac_window_menu_wins(&g_ms,wins,16);
                int yy=y-(m.y+8);
                if(yy>=0&&yy<n*22) mac_dispatch(&g_ms,ME_MENUCLICK,MACT_FOCUSWIN,wins[yy/22]);
                else{
                    int by=yy-(n*22+6);
                    if(by>=0&&by<22)mac_dispatch(&g_ms,ME_MENUCLICK,MACT_MINIMIZE,0);
                    else if(by>=22&&by<44)mac_dispatch(&g_ms,ME_MENUCLICK,MACT_FRONTALL,0);
                    else if(by>=44&&by<66)mac_dispatch(&g_ms,ME_MENUCLICK,MACT_FULLSCREEN,0);
                }
            }
        }
        g_menu_drawn=0; damage_ui(); gfx_damage(oldmenu); return;  /* 擦掉旧下拉框 */
    }
    /* 菜单栏 */
    if (y < MBAR_H) {
        int m=menubar_hit(x);
        if(m){ g_menu_drawn=1; g_menu_kind=m; mac_dispatch(&g_ms,ME_MENUOPEN,m==1?MMS_APPLE:MMS_APP,0); }
        else if(x>=gfx_width()-150) mac_dispatch(&g_ms,ME_SPOTLIGHT_TOGGLE,0,1);
        damage_ui(); if(m) gfx_damage(menu_rect()); return;  /* 绘制新下拉框 */
    }
    /* Dock */
    if (y >= gfx_height()-DOCK_H) {
        int wi=dock_win_at(x,y);
        if(wi>=0){ mac_dispatch(&g_ms,ME_DOCKRESTORE,wi,0); damage_ui(); return; }
        int a=dock_app_at(x,y);
        if(a>=0){ mac_dispatch(&g_ms,ME_DOCKCLICK,a,0); damage_ui(); return; }
        damage_ui(); return;
    }
    /* 窗口 */
    int wi=top_win_at(x,y);
    if (wi<0){ mac_dispatch(&g_ms,ME_CLICKDESKTOP,0,0); damage_ui(); return; }
    struct gfx_rect r={g_ms.wins[wi].b.x,g_ms.wins[wi].b.y,g_ms.wins[wi].b.w,g_ms.wins[wi].b.h};
    if(g_ms.wins[wi].state==MW_FULL){r.x=0;r.y=MBAR_H;r.w=gfx_width();r.h=gfx_height()-MBAR_H;}
    /* 记录旧矩形（含阴影）——关闭/最小化/全屏后该窗口可能消失或变形，
       damage_ui() 只登记当前仍可见的窗口，旧区域必须显式登记才能擦掉残影。 */
    struct gfx_rect oldwin = r;
    if (y < r.y+TITLE_H) {
        if(x>=r.x+10&&x<r.x+22) mac_dispatch(&g_ms,ME_CLOSEWIN,wi,0);
        else if(x>=r.x+30&&x<r.x+42) mac_dispatch(&g_ms,ME_MINWIN,wi,0);
        else if(x>=r.x+50&&x<r.x+62) mac_dispatch(&g_ms,ME_FULLSCREEN,wi,0);
        else { mac_dispatch(&g_ms,ME_FOCUSWIN,wi,0); if(on_titlebar(wi,x,y)){ g_drag_win=wi; g_drag_dx=x-g_ms.wins[wi].b.x; g_drag_dy=y-g_ms.wins[wi].b.y; g_drag_ox=g_ms.wins[wi].b.x; g_drag_oy=g_ms.wins[wi].b.y; } }
    } else mac_dispatch(&g_ms,ME_FOCUSWIN,wi,0);
    damage_pad(oldwin,12);   /* ★ 擦掉旧窗口/按钮区域，杜绝关闭残影 */
    damage_ui();
}

/* =========================================================================
 * 主循环
 * ========================================================================= */
void macdesktop_run(void) {
    /* 无帧缓冲时打印与原 desktop 完全相同的日志：golden 基线逐字一致（零扰动）。
       golden 环境刻意不挂 ramfb，桌面优雅降级为串口 shell，此处必须原样输出。 */
    if (!gfx_ready()) { kputs("[desktop] 无帧缓冲，保持串口 shell\n"); return; }
    gfx_take_screen();
    gfx_set_redraw(mac_redraw);
    if (!g_inited) {
        mac_init(&g_ms);
        /* 默认打开 Safari / 备忘录 / 访达 三窗，错开摆放 */
        mac_dispatch(&g_ms, ME_LAUNCH, MA_SAFARI, 0);
        mac_dispatch(&g_ms, ME_LAUNCH, MA_NOTES, 0);
        mac_dispatch(&g_ms, ME_LAUNCH, MA_FINDER, 0);
        static const short PX[3]={70,150,230}, PY[3]={54,96,138};
        int k=0;
        for(int i=0;i<MAC_NWIN;i++){
            if(!g_ms.wins[i].used)continue;
            g_ms.wins[i].b.x=PX[k%3]; g_ms.wins[i].b.y=PY[k%3];
            g_ms.wins[i].b.w=520; g_ms.wins[i].b.h=340; k++;
        }
        g_mx=gfx_width()/2; g_my=gfx_height()/2;
        g_inited=1;
    }
    gfx_damage(full());
    gfx_present();
    kputs("[macdesktop] macOS 风格桌面已就绪\n");
    kputs("[macdesktop] 点菜单栏/Dock/窗口；拖标题栏移动；红黄绿=关/最小化/全屏\n");
    kputs("[macdesktop] Ctrl+Tab 切 App，空格聚焦搜索，q 退出\n");

    for (;;) {
        usb_kbd_poll();
        if (desktop_ring_peek_uart()) break;
        if (desktop_exit_requested()) break;
        char c;
        while (desktop_ring_pop_kbd(&c)) {
            if (c=='q'||c=='Q'||c==3) goto done;
            if (c=='\t') { mac_dispatch(&g_ms,ME_CMDTAB,0,0); damage_ui(); }
            else if (c==' ') { mac_dispatch(&g_ms,ME_SPOTLIGHT_TOGGLE,0,1); gfx_damage(full()); }
            else if (c=='w'||c=='W') {
                if (g_ms.focused_win>=0) {
                    int fw=g_ms.focused_win;
                    struct gfx_rect ow={g_ms.wins[fw].b.x,g_ms.wins[fw].b.y,g_ms.wins[fw].b.w,g_ms.wins[fw].b.h};
                    mac_dispatch(&g_ms,ME_CLOSEWIN,fw,0);
                    damage_pad(ow,12);
                }
                damage_ui();
            }
            else if (c=='m'||c=='M') {
                if (g_ms.focused_win>=0) {
                    int fw=g_ms.focused_win;
                    struct gfx_rect ow={g_ms.wins[fw].b.x,g_ms.wins[fw].b.y,g_ms.wins[fw].b.w,g_ms.wins[fw].b.h};
                    mac_dispatch(&g_ms,ME_MINWIN,fw,0);
                    damage_pad(ow,12);
                }
                damage_ui();
            }
        }
        /* 坐标刷新：mouse_get 返回是否有新坐标；坐标变化即 moved 驱动拖动。
           按键电平不可靠（边沿被 mouse_pop_btn 消费），所以拖动不看 btn。 */
        int nx, ny, wheel=0, moved=0;
        if (mouse_get(&nx,&ny,0,&wheel)) {
            moved = (nx!=g_mx || ny!=g_my);
            if (moved) {
                /* 擦旧画新：旧光标矩形登记脏区让底层内容重绘覆盖旧箭头；
                   新光标矩形登记脏区让 mac_redraw 在新位置画出箭头。 */
                gfx_damage((struct gfx_rect){g_mx,g_my,CURSOR_BOX_W,CURSOR_BOX_H});
                g_mx=nx; g_my=ny;
                gfx_damage((struct gfx_rect){g_mx,g_my,CURSOR_BOX_W,CURSOR_BOX_H});
            } else { g_mx=nx; g_my=ny; }
        }
        /* 悬停检测：Dock 区域 */
        int nh=-1;
        if(g_my>=gfx_height()-DOCK_H){
            int xs[32],apps[32],wins[32];
            int n=dock_items(xs,apps,wins,32);
            for(int i=0;i<n;i++) if(apps[i]>=0&&wins[i]<0&&hit((struct gfx_rect){xs[i]-4,dock_y0()-8,DOCK_ICON+8,DOCK_ICON+8},g_mx,g_my)){nh=apps[i];break;}
        }
        if(nh!=g_hover_dock){
            g_hover_dock=nh;
            gfx_damage((struct gfx_rect){0,gfx_height()-DOCK_H-10,gfx_width(),DOCK_H+10});
        }
        /* 拖动窗口：坐标变化时更新位置，只登记旧/新两块脏区（含阴影），不整屏重绘 */
        if(g_drag_win>=0 && moved){
            struct mac_bounds *b=&g_ms.wins[g_drag_win].b;
            struct gfx_rect oldr={g_drag_ox,g_drag_oy,b->w,b->h};
            b->x=(short)(g_mx-g_drag_dx);
            b->y=(short)(g_my-g_drag_dy);
            /* 约束在屏内，标题栏不钻进菜单栏 */
            if (b->x < 0) b->x = 0;
            if (b->x > gfx_width() - 60) b->x = gfx_width() - 60;
            if (b->y < MBAR_H) b->y = MBAR_H;
            if (b->y > gfx_height() - TITLE_H) b->y = gfx_height() - TITLE_H;
            struct gfx_rect newr={b->x,b->y,b->w,b->h};
            damage_pad(oldr,10); damage_pad(newr,10);
            g_drag_ox=b->x; g_drag_oy=b->y;
        }
        /* 按键边沿：按下→on_press（可能在标题栏建立拖动）；松开→清拖动态 */
        int st=0;
        while (mouse_pop_btn(&st)) {
            int pressed=(st&1), released=!(st&1);
            if(pressed)  on_press(g_mx,g_my);
            if(released) g_drag_win=-1;
        }
        if (g_goto_classic) break;   /* 苹果菜单点 Classic Desktop → 退出，交调用方进原桌面 */
        gfx_present();
    }
done:
    gfxterm_stop();
    g_inited=1;
    fb_console_reset();
}

/* 供 kernel 层查询：上次 mac 桌面是否由「Classic Desktop」菜单项退出。
   返回后清零，避免影响下次进入。 */
int macdesktop_goto_classic(void)
{
    int v = g_goto_classic;
    g_goto_classic = 0;
    return v;
}
