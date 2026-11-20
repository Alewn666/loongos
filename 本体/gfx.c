#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS 桌面路线 P1 · 绘图实现层（gfx.c）
 *
 * 职责：把 gfx.h 的纯函数（裁剪/相交/脏矩形合并）接到真实帧缓冲上。
 *
 * 刻意不做的事（都是项目既有约束）：
 *   - 不引 kmalloc：裁剪栈/脏矩形表全是固定容量静态数组，BSS 增量 < 1KB；
 *   - 不复制字库：font8x16.h / font_cjk.h 只在 kernel.c 里有一份（CJK 885 字形
 *     ≈ 32KB），本文件通过 fb_query_glyph() 查询位图指针，自己逐像素画；
 *   - 不做全屏双缓冲：TCG 下 800x600x4 的整屏拷贝太慢，只按脏矩形重绘。
 *
 * 帧缓冲像素格式：0x00RRGGBB（低 24 位 RGB，与 fb_putc 一致），每行 stride
 * 个 u32（stride 是**像素数**不是字节数，见 bootinfo）。
 */
#include <stdint.h>
#include "gfx.h"
#include "gfx_draw.h"

typedef uint32_t u32;
typedef uint8_t  u8;

/* kernel.c 导出：查询字形位图（不拷贝）。返回 0 = 找到。
   row_bytes：ASCII 为 1（8 像素/行），CJK 为 2（16 像素/行，高低字节各 8 像素） */

static volatile u32 *G_fb;
static int G_w, G_h, G_stride;
static int G_owned;                       /* 屏幕是否被桌面接管 */

/* 裁剪栈：stack[0] 恒为全屏，push 时与栈顶求交 */
static struct gfx_rect G_clip[GFX_CLIP_DEPTH];
static int G_clip_n;

/* 脏矩形表（gfx.h 的 gfx_dirty_add 负责合并与表满退化） */
static struct gfx_rect G_dirty[GFX_MAX_DIRTY];
static int G_dirty_n;
static void (*G_redraw)(struct gfx_rect r);

void gfx_init(void *fb, int w, int h, int stride)
{
    G_fb = (volatile u32 *)fb;
    G_w = w; G_h = h; G_stride = stride ? stride : w;
    G_clip_n = 1;
    G_clip[0] = (struct gfx_rect){0, 0, w, h};
    G_dirty_n = 0;
    G_owned = 0;
}

int gfx_ready(void)      { return G_fb != 0 && G_w > 0 && G_h > 0; }
int gfx_width(void)      { return G_w; }
int gfx_height(void)     { return G_h; }
int gfx_screen_owned(void) { return G_owned; }

void gfx_take_screen(void)
{
    if (!gfx_ready()) return;
    G_owned = 1;
    G_clip_n = 1;
    G_clip[0] = (struct gfx_rect){0, 0, G_w, G_h};
    G_dirty_n = 0;
}

void gfx_release_screen(void)
{
    G_owned = 0;
    G_clip_n = 1;
    G_dirty_n = 0;
}

/* ---- 裁剪栈 ---- */
void gfx_clip_push(struct gfx_rect r)
{
    if (G_clip_n >= GFX_CLIP_DEPTH) {
        /* 栈满：不再加新层，改为把新裁剪与栈顶取交 —— 保持「裁剪单调收紧」，
         * 且 push/pop 始终一一对应（消除旧实现「满栈 push 不计数却仍 pop」的不对称）。 */
        G_clip[G_clip_n - 1] = gfx_intersect(G_clip[G_clip_n - 1], r);
        return;
    }
    struct gfx_rect cur = G_clip[G_clip_n - 1];
    G_clip[G_clip_n++] = gfx_intersect(cur, r);
}
void gfx_clip_pop(void)
{
    if (G_clip_n > 1) G_clip_n--;
}

struct gfx_rect gfx_clip_current(void) { return G_clip[G_clip_n - 1]; }

/* 把矩形裁剪到「当前裁剪 ∩ 屏幕」；返回空矩形表示无需绘制 */
static struct gfx_rect drawable(struct gfx_rect r)
{
    r = gfx_intersect(G_clip[G_clip_n - 1], r);
    r = gfx_clip(r, G_w, G_h);
    return r;
}

static inline void px(int x, int y, u32 c)
{
    G_fb[(uint32_t)y * (uint32_t)G_stride + (uint32_t)x] = c;
}

/* 第四十七轮：批量填充走非 volatile 视图 + 64 位成对写。
 * 原实现逐像素写 volatile u32，每次都是一条独立 store，且编译器不能合并；
 * 800x600 全屏要 48 万次 4 字节写（实测 1072us/屏，比 fb_clear_all 慢 4.8 倍）。
 * 帧缓冲在 MMU 里是 Normal-WB（见 kernel.c PROT_FB_BLK），CPU 视角就是普通内存，
 * 用 u64 写不会丢像素；桌面 present 由 gfx_present 统一提交，这里只需编译器屏障
 * 防止把填充挪到后面的绘制之后（单核顺序足够，无 DMA 读帧缓冲）。 */
void gfx_fill(struct gfx_rect r, u32 rgb)
{
    if (!G_fb) return;
    r = drawable(r);
    if (!gfx_rect_valid(r)) return;
    uint32_t *base = (uint32_t *)(uintptr_t)G_fb;
    u64 pat = ((u64)rgb << 32) | (u64)rgb;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *p = base + (u64)(u32)y * (u32)G_stride + (u32)r.x;
        int n = r.w;
        if ((uintptr_t)p & 4) { *p++ = rgb; n--; }      /* 头部 4B 对齐 */
        u64 *q = (u64 *)p;
        while (n >= 16) {                               /* 主体：128B/轮，成对 16B 写 */
            q[0] = pat; q[1] = pat; q[2] = pat; q[3] = pat;
            q[4] = pat; q[5] = pat; q[6] = pat; q[7] = pat;
            q += 8; n -= 16;
        }
        while (n >= 2) { *q++ = pat; n -= 2; }
        if (n) *(uint32_t *)q = rgb;                    /* 尾巴 1 像素 */
    }
    __asm__ volatile("" ::: "memory");
}

void gfx_rect_draw(struct gfx_rect r, u32 rgb, int thickness)
{
    if (thickness < 1) thickness = 1;
    r = gfx_rect_norm(r);
    gfx_fill((struct gfx_rect){r.x, r.y, r.w, thickness}, rgb);                    /* 上 */
    gfx_fill((struct gfx_rect){r.x, r.y + r.h - thickness, r.w, thickness}, rgb);  /* 下 */
    gfx_fill((struct gfx_rect){r.x, r.y, thickness, r.h}, rgb);                    /* 左 */
    gfx_fill((struct gfx_rect){r.x + r.w - thickness, r.y, thickness, r.h}, rgb);  /* 右 */
}

void gfx_line(int x0, int y0, int x1, int y1, u32 rgb)
{
    if (!G_fb) return;
    /* Bresenham；裁剪矩形只算一次，逐点用 gfx_hit 判定。
       原实现每个点都构造矩形再走 gfx_clip，壁纸 7 条斜线要算上千次裁剪。 */
    struct gfx_rect c = gfx_clip(G_clip[G_clip_n - 1], G_w, G_h);
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        if (gfx_hit(c, x0, y0)) px(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* =========================================================================
 * 桌面质感图元：圆角 / 实心圆 / 垂直渐变 / 半透明混合
 * 全部复用 drawable() 裁剪，不占静态大数组；渐变与圆角按行 gfx_fill 批量写。
 * 半透明混合是逐像素读改写，只用于菜单栏/Dock/窗口等局部，不做全屏。
 * ========================================================================= */

static inline u32 getpx(int x, int y)
{
    return G_fb[(uint32_t)y * (uint32_t)G_stride + (uint32_t)x];
}

/* 小整数开方（半径一般 <= 32，线性探测足够，避免 libm） */
static int isqrt_small(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* 圆角实心矩形：每行按圆方程算左右缩进，中部整宽行批量填充 */
void gfx_roundrect(struct gfx_rect r, u32 rgb, int rad)
{
    if (!G_fb) return;
    r = drawable(r);
    if (!gfx_rect_valid(r)) return;
    if (rad < 0) rad = 0;
    if (rad > r.w / 2) rad = r.w / 2;
    if (rad > r.h / 2) rad = r.h / 2;
    if (rad == 0) { gfx_fill(r, rgb); return; }
    if (r.h > 2 * rad)
        gfx_fill((struct gfx_rect){r.x, r.y + rad, r.w, r.h - 2 * rad}, rgb);
    int rr = rad * rad;
    for (int y = 0; y < rad; y++) {
        int yy = rad - 1 - y;
        int off = rad - isqrt_small(rr - yy * yy);
        int w = r.w - 2 * off;
        if (w <= 0) continue;
        gfx_fill((struct gfx_rect){r.x + off, r.y + y, w, 1}, rgb);
        gfx_fill((struct gfx_rect){r.x + off, r.y + r.h - 1 - y, w, 1}, rgb);
    }
}

/* 实心圆（中点整数法，按水平跨度批量填充，用于红黄绿按钮/运行圆点/图标） */
void gfx_filled_circle(int cx, int cy, int rad, u32 rgb)
{
    if (!G_fb || rad < 0) return;
    int x = rad, y = 0, err = 1 - rad;
    while (x >= y) {
        gfx_fill((struct gfx_rect){cx - x, cy + y, 2 * x + 1, 1}, rgb);
        gfx_fill((struct gfx_rect){cx - x, cy - y, 2 * x + 1, 1}, rgb);
        gfx_fill((struct gfx_rect){cx - y, cy + x, 2 * y + 1, 1}, rgb);
        gfx_fill((struct gfx_rect){cx - y, cy - x, 2 * y + 1, 1}, rgb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* 柔和径向光晕：alpha 从中心 max_a 平滑（二次衰减）到边缘 0。
   仅遍历裁剪区与圆的交集，逐像素读改写；用于壁纸光晕，避免实心圆硬边。 */
void gfx_soft_circle(int cx, int cy, int rad, u32 rgb, int max_a)
{
    if (!G_fb || rad <= 0 || max_a <= 0) return;
    struct gfx_rect clip = gfx_clip(G_clip[G_clip_n - 1], G_w, G_h);
    int x0 = cx - rad, x1 = cx + rad, y0 = cy - rad, y1 = cy + rad;
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 >= clip.x + clip.w) x1 = clip.x + clip.w - 1;
    if (y1 >= clip.y + clip.h) y1 = clip.y + clip.h - 1;
    if (x0 > x1 || y0 > y1) return;
    int sr=(int)((rgb>>16)&255), sg=(int)((rgb>>8)&255), sb=(int)(rgb&255);
    int rr = rad*rad;
    uint32_t *base = (uint32_t *)(uintptr_t)G_fb;
    for (int y=y0; y<=y1; y++) {
        int dy = y - cy;
        uint32_t *row = base + (u64)(u32)y * (u32)G_stride;
        for (int x=x0; x<=x1; x++) {
            int dx = x - cx;
            int d2 = dx*dx + dy*dy;
            if (d2 >= rr) continue;
            /* t=0 中心..1 边缘；alpha=max_a*(1-t^2)，边缘平滑归零 */
            int t2 = (d2 * 256) / rr;             /* 0..255 */
            int a = max_a - (max_a * t2) / 256;   /* max_a..0 */
            if (a <= 0) continue;
            u32 d = row[x];
            int dr=(int)((d>>16)&255), dg=(int)((d>>8)&255), db=(int)(d&255);
            row[x] = GFX_RGB((sr*a + dr*(255-a))/255,
                             (sg*a + dg*(255-a))/255,
                             (sb*a + db*(255-a))/255);
        }
    }
}

/* 垂直线性渐变：逐行插值后整行批量填充（壁纸/标题栏/按钮立体感） */
void gfx_vgrad(struct gfx_rect r, u32 top, u32 bottom)
{
    if (!G_fb) return;
    r = drawable(r);
    if (!gfx_rect_valid(r)) return;
    int h = r.h;
    int r0=(int)((top>>16)&255), g0=(int)((top>>8)&255), b0=(int)(top&255);
    int r1=(int)((bottom>>16)&255), g1=(int)((bottom>>8)&255), b1=(int)(bottom&255);
    for (int i = 0; i < h; i++) {
        int t = h > 1 ? (i * 255) / (h - 1) : 0;
        u32 c = GFX_RGB(r0 + (r1 - r0) * t / 255,
                        g0 + (g1 - g0) * t / 255,
                        b0 + (b1 - b0) * t / 255);
        gfx_fill((struct gfx_rect){r.x, r.y + i, r.w, 1}, c);
    }
}

/* 半透明矩形混合：out = src*a + dst*(1-a)，alpha 0..255。局部使用。 */
void gfx_blend(struct gfx_rect r, u32 rgb, int alpha)
{
    if (!G_fb || alpha <= 0) return;
    if (alpha >= 255) { gfx_fill(r, rgb); return; }
    r = drawable(r);
    if (!gfx_rect_valid(r)) return;
    uint32_t *base = (uint32_t *)(uintptr_t)G_fb;
    int sr=(int)((rgb>>16)&255), sg=(int)((rgb>>8)&255), sb=(int)(rgb&255);
    int ia = 255 - alpha;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *p = base + (u64)(u32)y * (u32)G_stride + (u32)r.x;
        for (int i = 0; i < r.w; i++, p++) {
            u32 d = *p;
            int dr=(int)((d>>16)&255), dg=(int)((d>>8)&255), db=(int)(d&255);
            *p = GFX_RGB((sr*alpha + dr*ia)/255,
                         (sg*alpha + dg*ia)/255,
                         (sb*alpha + db*ia)/255);
        }
    }
    __asm__ volatile("" ::: "memory");
}

/* 圆角半透明矩形（毛玻璃菜单栏/Dock 底板）：逐行算缩进后 blend 单行 */
void gfx_blend_roundrect(struct gfx_rect r, u32 rgb, int rad, int alpha)
{
    if (!G_fb || alpha <= 0) return;
    r = gfx_intersect(G_clip[G_clip_n - 1], r);
    r = gfx_clip(r, G_w, G_h);
    if (!gfx_rect_valid(r)) return;
    if (rad < 0) rad = 0;
    if (rad > r.w / 2) rad = r.w / 2;
    if (rad > r.h / 2) rad = r.h / 2;
    if (rad == 0) { gfx_blend(r, rgb, alpha); return; }
    if (r.h > 2 * rad)
        gfx_blend((struct gfx_rect){r.x, r.y + rad, r.w, r.h - 2 * rad}, rgb, alpha);
    int rr = rad * rad;
    for (int y = 0; y < rad; y++) {
        int yy = rad - 1 - y;
        int off = rad - isqrt_small(rr - yy * yy);
        int w = r.w - 2 * off;
        if (w <= 0) continue;
        gfx_blend((struct gfx_rect){r.x + off, r.y + y, w, 1}, rgb, alpha);
        gfx_blend((struct gfx_rect){r.x + off, r.y + r.h - 1 - y, w, 1}, rgb, alpha);
    }
}

/* ---- UTF-8 解码（只用于量宽度与逐码点绘制，不攒状态） ---- */
/* 返回码点、把 *s 推进到下一个字符；非法序列返回 '?' 且只推进 1 字节 */
static u32 u8_next(const char **s)
{
    const u8 *p = (const u8 *)*s;
    u8 b = *p;
    if (b < 0x80) { *s += 1; return b; }
    int need;
    u32 cp;
    if ((b & 0xE0) == 0xC0)      { need = 1; cp = b & 0x1Fu; }
    else if ((b & 0xF0) == 0xE0) { need = 2; cp = b & 0x0Fu; }
    else if ((b & 0xF8) == 0xF0) { need = 3; cp = b & 0x07u; }
    else { *s += 1; return '?'; }
    *s += 1;
    for (int i = 0; i < need; i++) {
        u8 c = (u8)**s;
        if ((c & 0xC0) != 0x80) return '?';   /* 坏序列：丢掉，不越界读 */
        cp = (cp << 6) | (c & 0x3Fu);
        *s += 1;
    }
    return cp;
}

int gfx_text_w(const char *s)
{
    int w = 0;
    while (*s) {
        u32 cp = u8_next(&s);
        w += (cp < 128) ? 8 : 16;
    }
    return w;
}

/* 画一个码点（含裁剪与背景填充）。返回绘制后的 x。 */
static int glyph(int x, int y, u32 cp, u32 fg, u32 bg)
{
    const u8 *bmp; int w = 0, h = 0, rb = 0;
    if (fb_query_glyph(cp, &bmp, &w, &h, &rb) != 0) { w = 8; h = 16; bmp = 0; }
    /* 第四十七轮：整个字形只算一次可视范围，不再逐像素 gfx_clip。
       原实现每个亮点都要构造 1x1 矩形 → gfx_clip → gfx_hit，一个 8x16 字形
       最多 128 次裁剪；桌面满屏文字实测 9.5us/字，是渲染最大头。 */
    struct gfx_rect vis = gfx_clip(G_clip[G_clip_n - 1], G_w, G_h);
    struct gfx_rect gr = {x, y, w, h};
    if (!gfx_overlap(vis, gr)) return x + w;      /* 整字在裁剪区外：零像素开销 */
    if (bg != 0xFFFFFFFFu)
        gfx_fill(gr, bg);
    if (bmp && G_fb) {
        int cx0 = x > vis.x ? x : vis.x;
        int cy0 = y > vis.y ? y : vis.y;
        int cx1 = (x + w) < (vis.x + vis.w) ? (x + w) : (vis.x + vis.w);
        int cy1 = (y + h) < (vis.y + vis.h) ? (y + h) : (vis.y + vis.h);
        uint32_t *base = (uint32_t *)(uintptr_t)G_fb;
        for (int yy = cy0; yy < cy1; yy++) {
            const u8 *rowb = bmp + (yy - y) * rb;
            uint32_t *dst = base + (u64)(u32)yy * (u32)G_stride;
            for (int xx = cx0; xx < cx1; xx++) {
                int c = xx - x;
                if ((rowb[c >> 3] >> (7 - (c & 7))) & 1) dst[xx] = fg;
            }
        }
        __asm__ volatile("" ::: "memory");
    }
    return x + w;
}

int gfx_text(int x, int y, const char *s, u32 fg, u32 bg)
{
    /* 第四十七轮：先按字符宽度做一次矩形相交，完全在裁剪区外的 ASCII 直接跳过。
       桌面输入行刷新时脏区只有 22px 高，终端却会把 64 行历史都走一遍；
       原实现每个字符都要 fb_query_glyph（CJK 是 885 字形二分）再算裁剪，
       一屏文字白算上千次。ASCII 宽度固定 8，可以安全跳过而不影响排版。 */
    struct gfx_rect vis = gfx_clip(G_clip[G_clip_n - 1], G_w, G_h);
    while (*s) {
        u32 cp = u8_next(&s);
        if (cp < 128) {
            if (x + 8 <= vis.x || x >= vis.x + vis.w ||
                y + 16 <= vis.y || y >= vis.y + vis.h) {
                x += 8;
                continue;
            }
        }
        x = glyph(x, y, cp, fg, bg);
    }
    return x;
}

/* ---- 脏矩形 ---- */
void gfx_set_redraw(void (*fn)(struct gfx_rect r)) { G_redraw = fn; }

void gfx_damage(struct gfx_rect r)
{
    gfx_dirty_add(G_dirty, &G_dirty_n, GFX_MAX_DIRTY, r, G_w, G_h);
}

void gfx_present(void)
{
    if (!G_redraw || !G_dirty_n) return;
    /* 快照计数：redraw 回调里可能再调 gfx_damage（例如时钟刷新），
       先取当前批次，避免边遍历边改表导致重入。 */
    struct gfx_rect batch[GFX_MAX_DIRTY];
    int n = G_dirty_n;
    if (n > GFX_MAX_DIRTY) n = GFX_MAX_DIRTY;
    for (int i = 0; i < n; i++) batch[i] = G_dirty[i];
    G_dirty_n = 0;
    for (int i = 0; i < n; i++) {
        gfx_clip_push(batch[i]);
        G_redraw(batch[i]);
        gfx_clip_pop();
    }
}
