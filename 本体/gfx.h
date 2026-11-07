/* LoongOS 桌面路线 P1 · 图形纯函数层（gfx.h）
 *
 * 定位：窗口管理的「可测内核」，故意不碰任何硬件、不引 kmalloc、不占内核 BSS。
 *   - 全部 static inline，无全局变量、无静态数组；
 *   - 数据结构由调用方以静态数组提供（符合「静态窗口池 / 固定上限脏矩形表」的预算约束）；
 *   - 纯函数：同样的输入永远同样的输出，可直接在宿主 x86 上 gcc 单测（见 test_wm_host.c）。
 *
 * 为什么先做这一层（2026-11-07）：
 *   项目铁律是「NAS 上不起 QEMU」，图形效果只能靠 VM 实测；而裁剪、命中、z 序、
 *   脏矩形合并这几类逻辑一旦写错，在真机上表现为「点不中 / 窗口乱盖 / 局部不刷新」，
 *   靠回传截图极难定位。把它们抽成纯函数后，改渲染器之前先在宿主上把逻辑跑对。
 *
 * 坐标约定：屏幕左上为 (0,0)，矩形 [x, x+w) × [y, y+h)，w/h 必须 > 0。
 *           所有输入先经 gfx_rect_norm 规范化，避免负宽高把命中判断绕晕。
 *           计算全程用 int64 防溢出（脏矩形并集可能跨整屏）。
 */
#ifndef LOONGOS_GFX_H
#define LOONGOS_GFX_H

#include <stdint.h>

struct gfx_rect { int x, y, w, h; };
struct gfx_point { int x, y; };

#define GFX_MAX_DIRTY 32   /* 脏矩形表固定上限：满了就退化为整屏重绘，绝不动态分配 */

/* ---- 基础 ---- */

/* 规范化：把负宽高翻正（右/下对齐到左上），零宽高保持零 */
static inline struct gfx_rect gfx_rect_norm(struct gfx_rect r)
{
    if (r.w < 0) { r.x += r.w; r.w = -r.w; }
    if (r.h < 0) { r.y += r.h; r.h = -r.h; }
    return r;
}

static inline int gfx_rect_valid(struct gfx_rect r)
{
    return r.w > 0 && r.h > 0;
}

/* 点是否落在矩形内（左闭右开、上闭下开，与像素栅格一致） */
static inline int gfx_hit(struct gfx_rect r, int px, int py)
{
    r = gfx_rect_norm(r);
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

/* 裁剪到屏幕：返回裁剪后的矩形；完全在屏外则返回 w=0（调用方跳过绘制） */
static inline struct gfx_rect gfx_clip(struct gfx_rect r, int sw, int sh)
{
    r = gfx_rect_norm(r);
    int64_t x0 = r.x < 0 ? 0 : r.x;
    int64_t y0 = r.y < 0 ? 0 : r.y;
    int64_t x1 = (int64_t)r.x + r.w, y1 = (int64_t)r.y + r.h;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x0 || y1 <= y0) { struct gfx_rect e = {0, 0, 0, 0}; return e; }
    struct gfx_rect o = {(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
    return o;
}

/* 两矩形是否相交（含边界相切算不相交） */
static inline int gfx_overlap(struct gfx_rect a, struct gfx_rect b)
{
    a = gfx_rect_norm(a); b = gfx_rect_norm(b);
    if (!gfx_rect_valid(a) || !gfx_rect_valid(b)) return 0;
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

/* 交集；不相交返回 w=0 */
static inline struct gfx_rect gfx_intersect(struct gfx_rect a, struct gfx_rect b)
{
    a = gfx_rect_norm(a); b = gfx_rect_norm(b);
    int64_t x0 = a.x > b.x ? a.x : b.x;
    int64_t y0 = a.y > b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0) { struct gfx_rect e = {0, 0, 0, 0}; return e; }
    struct gfx_rect o = {(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
    return o;
}

/* 包围盒并集（脏矩形合并用）；空矩形自动忽略 */
static inline struct gfx_rect gfx_union(struct gfx_rect a, struct gfx_rect b)
{
    a = gfx_rect_norm(a); b = gfx_rect_norm(b);
    if (!gfx_rect_valid(a)) return b;
    if (!gfx_rect_valid(b)) return a;
    int64_t x0 = a.x < b.x ? a.x : b.x;
    int64_t y0 = a.y < b.y ? a.y : b.y;
    int64_t ax1 = (int64_t)a.x + a.w, bx1 = (int64_t)b.x + b.w;
    int64_t ay1 = (int64_t)a.y + a.h, by1 = (int64_t)b.y + b.h;
    int64_t x1 = ax1 > bx1 ? ax1 : bx1, y1 = ay1 > by1 ? ay1 : by1;
    struct gfx_rect o = {(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
    return o;
}

/* ---- z 序 ----
 * 窗口数组由调用方提供（静态池），z 越大越靠上，彼此不同但允许乱序存放。
 * gfx_pick_top 返回「点 (px,py) 命中的最上层窗口下标」，没有命中返回 -1。
 * 这是纯查找，不改数组（不会在中断/渲染之间引入状态竞争）。
 */
struct gfx_win { struct gfx_rect r; int z; };

static inline int gfx_pick_top(const struct gfx_win *wins, int n, int px, int py)
{
    int best = -1, best_z = 0;
    for (int i = 0; i < n; i++) {
        if (!gfx_hit(wins[i].r, px, py)) continue;
        if (best < 0 || wins[i].z > best_z) { best = i; best_z = wins[i].z; }
    }
    return best;
}

/* 把 win 提到最顶：返回新的 z 值（当前最大 z + 1），调用方自行写回。
 * n<=0 时返回 1，保证第一个窗口的 z 从 1 开始、0 留给「桌面」。
 */
static inline int gfx_top_z(const struct gfx_win *wins, int n)
{
    int m = 0;
    for (int i = 0; i < n; i++) if (wins[i].z > m) m = wins[i].z;
    return m + 1;
}

/* ---- 脏矩形表 ----
 * 由调用方提供固定容量数组 dirty[GFX_MAX_DIRTY] 与计数 *n。
 * 策略（BSS 预算优先，不做精确矩形减法）：
 *   1. 新矩形若与已有项相交 → 合并成包围盒，可能产生一点过度重绘，但表不会膨胀；
 *   2. 合并后若又与别的项相交 → 继续合并（一轮收敛循环，最多 n 次）；
 *   3. 表满且仍有不相交的新矩形 → 退化：把整屏 (0,0,sw,sh) 当作唯一脏区，
 *      返回 1 表示「整屏重绘」。这样最坏情况也只是慢，不会漏画、不会越界、不分配内存。
 * 返回值：0 = 正常加入/合并；1 = 已退化为整屏重绘。
 */
static inline int gfx_dirty_add(struct gfx_rect *dirty, int *n, int cap,
                                struct gfx_rect add, int sw, int sh)
{
    if (cap > GFX_MAX_DIRTY) cap = GFX_MAX_DIRTY;
    add = gfx_clip(add, sw, sh);
    if (!gfx_rect_valid(add)) return 0;          /* 完全在屏外，无需重绘 */

    /* 反复与相交项合并，直到没有相交或只剩一项 */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < *n; i++) {
            if (!gfx_overlap(dirty[i], add)) continue;
            add = gfx_union(dirty[i], add);
            /* 用最后一项覆盖被合并的槽，保持数组紧凑 */
            dirty[i] = dirty[*n - 1];
            (*n)--;
            changed = 1;
            break;
        }
    }
    if (*n < cap) {
        dirty[(*n)++] = add;
        return 0;
    }
    /* 表满：整屏兜底 */
    struct gfx_rect full = {0, 0, sw, sh};
    dirty[0] = full;
    *n = 1;
    return 1;
}

#endif /* LOONGOS_GFX_H */
