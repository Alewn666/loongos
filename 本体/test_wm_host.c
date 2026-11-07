/* LoongOS 桌面路线 P1 · 宿主单测（不依赖内核、不需要 QEMU）
 *
 * 编译（在项目根目录）：
 *   cc -O1 -g -Wall -Wextra -Werror test_wm_host.c -o /tmp/test_wm_host && /tmp/test_wm_host
 *
 * 覆盖：矩形规范化 / 裁剪 / 命中 / 相交与交集 / 并集 / z 序拾取 /
 *       脏矩形合并与「表满退化为整屏」的兜底。
 * 全部为纯函数测试，任何一项失败都以非 0 退出，便于接进 CI。
 */
#include <stdio.h>
#include "gfx.h"

static int ok = 0, fail = 0;

#define CHECK(cond, name) do {                                        \
    if (cond) { ok++; printf("  [ok] %s\n", name); }                 \
    else      { fail++; printf("  [FAIL] %s  (line %d)\n", name, __LINE__); } \
} while (0)

static struct gfx_rect R(int x, int y, int w, int h)
{
    struct gfx_rect r = {x, y, w, h};
    return r;
}

int main(void)
{
    printf("== 矩形规范化 ==\n");
    {
        struct gfx_rect n = gfx_rect_norm(R(100, 50, -40, -20));
        CHECK(n.x == 60 && n.y == 30 && n.w == 40 && n.h == 20, "负宽高翻正且锚点左移");
        CHECK(!gfx_rect_valid(R(0, 0, 0, 10)), "零宽矩形判为无效");
        CHECK(gfx_rect_valid(R(0, 0, 1, 1)), "1x1 矩形有效");
    }

    printf("== 点命中（左闭右开）==\n");
    {
        struct gfx_rect r = R(10, 10, 20, 20);   /* x∈[10,30) y∈[10,30) */
        CHECK(gfx_hit(r, 10, 10), "左上角命中");
        CHECK(gfx_hit(r, 29, 29), "右下内侧命中");
        CHECK(!gfx_hit(r, 30, 10), "右边界不算命中");
        CHECK(!gfx_hit(r, 10, 30), "下边界不算命中");
        CHECK(!gfx_hit(r, 9, 15), "左侧外部不命中");
        CHECK(gfx_hit(R(30, 30, -20, -20), 15, 15), "负宽高矩形命中判断自动规范化");
    }

    printf("== 裁剪到屏幕 ==\n");
    {
        struct gfx_rect c = gfx_clip(R(-10, -10, 30, 30), 100, 100);
        CHECK(c.x == 0 && c.y == 0 && c.w == 20 && c.h == 20, "左侧/上方越界被裁掉");
        c = gfx_clip(R(90, 90, 30, 30), 100, 100);
        CHECK(c.x == 90 && c.y == 90 && c.w == 10 && c.h == 10, "右侧/下方越界被裁掉");
        c = gfx_clip(R(200, 200, 10, 10), 100, 100);
        CHECK(!gfx_rect_valid(c), "完全在屏外返回空矩形");
        c = gfx_clip(R(5, 5, 10, 10), 100, 100);
        CHECK(c.x == 5 && c.y == 5 && c.w == 10 && c.h == 10, "屏内矩形原样保留");
    }

    printf("== 相交 / 交集 ==\n");
    {
        struct gfx_rect a = R(0, 0, 10, 10), b = R(5, 5, 10, 10);
        CHECK(gfx_overlap(a, b), "部分重叠判为相交");
        struct gfx_rect i = gfx_intersect(a, b);
        CHECK(i.x == 5 && i.y == 5 && i.w == 5 && i.h == 5, "交集坐标正确");
        CHECK(!gfx_overlap(a, R(10, 0, 5, 5)), "边界相切不算相交");
        CHECK(!gfx_rect_valid(gfx_intersect(a, R(50, 50, 1, 1))), "不相交返回空矩形");
        CHECK(gfx_overlap(R(0, 0, 10, 10), R(1, 1, 2, 2)), "内含也算相交");
    }

    printf("== 并集 ==\n");
    {
        struct gfx_rect u = gfx_union(R(0, 0, 10, 10), R(20, 5, 10, 10));
        CHECK(u.x == 0 && u.y == 0 && u.w == 30 && u.h == 15, "包围盒并集正确");
        struct gfx_rect e = R(0, 0, 0, 0);
        CHECK(gfx_rect_valid(gfx_union(e, R(3, 4, 5, 6))), "空矩形与有效矩形并集保留有效项");
    }

    printf("== z 序拾取 ==\n");
    {
        struct gfx_win w[4] = {
            {R(0,   0, 100, 100), 1},   /* 桌面 */
            {R(10, 10,  50,  50), 2},   /* 窗口 A */
            {R(30, 30,  50,  50), 3},   /* 窗口 B（压在 A 上） */
            {R(200,200, 10,  10), 9},   /* 高 z 但在别处 */
        };
        CHECK(gfx_pick_top(w, 4, 15, 15) == 1, "只命中 A → 返回 A");
        CHECK(gfx_pick_top(w, 4, 40, 40) == 2, "A/B 都命中 → 返回 z 更大的 B");
        CHECK(gfx_pick_top(w, 4, 90, 90) == 0, "只命中桌面 → 返回桌面");
        CHECK(gfx_pick_top(w, 4, 500, 500) == -1, "谁都没命中 → -1");
        CHECK(gfx_pick_top(w, 0, 10, 10) == -1, "空窗口表 → -1");
        CHECK(gfx_top_z(w, 4) == 10, "置顶 z = 当前最大 9 + 1");
        CHECK(gfx_top_z(w, 0) == 1, "空表首个 z 从 1 开始");
    }

    printf("== 脏矩形表 ==\n");
    {
        struct gfx_rect d[GFX_MAX_DIRTY];
        int n = 0;
        CHECK(gfx_dirty_add(d, &n, GFX_MAX_DIRTY, R(0, 0, 10, 10), 800, 600) == 0 && n == 1,
              "首个脏区入表，n=1");
        CHECK(gfx_dirty_add(d, &n, GFX_MAX_DIRTY, R(100, 100, 10, 10), 800, 600) == 0 && n == 2,
              "不相交脏区各自入表，n=2");
        CHECK(gfx_dirty_add(d, &n, GFX_MAX_DIRTY, R(5, 5, 10, 10), 800, 600) == 0 && n == 2,
              "与首个相交 → 合并，n 不增长");
        CHECK(d[1].x == 0 && d[1].y == 0 && d[1].w == 15 && d[1].h == 15,
              "合并结果是正确的包围盒");
        CHECK(gfx_dirty_add(d, &n, GFX_MAX_DIRTY, R(700, 500, 200, 200), 800, 600) == 0,
              "越界脏区先裁剪再加入");
        CHECK(gfx_dirty_add(d, &n, GFX_MAX_DIRTY, R(-50, -50, 10, 10), 800, 600) == 0,
              "完全在屏外的脏区被忽略，不改变 n");
    }
    {
        printf("-- 表满退化为整屏兜底 --\n");
        struct gfx_rect d[GFX_MAX_DIRTY];
        int n = 0, rc = 0;
        /* 用互不相交的小格子把表填满 */
        for (int i = 0; i < GFX_MAX_DIRTY; i++)
            rc |= gfx_dirty_add(d, &n, 4, R(i * 20, 0, 10, 10), 800, 600);
        CHECK(rc == 1, "容量 4 填到第 5 个不相交脏区时触发退化");
        CHECK(n == 1 && d[0].x == 0 && d[0].y == 0 && d[0].w == 800 && d[0].h == 600,
              "退化为唯一的全屏脏区，不越界、不分配内存");
        CHECK(gfx_dirty_add(d, &n, 4, R(1, 1, 1, 1), 800, 600) == 0,
              "退化后新脏区与全屏相交 → 正常合并，不再增长");
    }

    printf("\n结果: %d 通过 / %d 失败\n", ok, fail);
    return fail ? 1 : 0;
}
