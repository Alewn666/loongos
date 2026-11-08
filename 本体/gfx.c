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
extern int fb_query_glyph(u32 cp, const u8 **bmp, int *w, int *h, int *row_bytes);

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
    if (G_clip_n >= GFX_CLIP_DEPTH) return;   /* 栈满：忽略新层（保守，不会画到界外） */
    struct gfx_rect cur = G_clip[G_clip_n - 1];
    G_clip[G_clip_n++] = gfx_intersect(cur, r);
}
void gfx_clip_pop(void)
{
    if (G_clip_n > 1) G_clip_n--;
}

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

/* ---- 图元 ---- */
void gfx_fill(struct gfx_rect r, u32 rgb)
{
    if (!G_fb) return;
    r = drawable(r);
    if (!gfx_rect_valid(r)) return;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            px(x, y, rgb);
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
    /* Bresenham；每个点都过 drawable 裁剪，避免逐点判断屏幕边界的重复代码 */
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        struct gfx_rect one = gfx_clip((struct gfx_rect){x0, y0, 1, 1}, G_w, G_h);
        if (gfx_rect_valid(one) && gfx_hit(G_clip[G_clip_n - 1], x0, y0))
            px(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
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
    if (bg != 0xFFFFFFFFu)
        gfx_fill((struct gfx_rect){x, y, w, h}, bg);
    if (bmp && G_fb) {
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
                u8 bits = bmp[r * rb + c / 8];
                if (!((bits >> (7 - (c & 7))) & 1)) continue;
                /* 逐点裁剪：文字可能被窗口边框裁掉半个字符 */
                if (!gfx_rect_valid(gfx_clip((struct gfx_rect){x + c, y + r, 1, 1}, G_w, G_h))) continue;
                if (!gfx_hit(G_clip[G_clip_n - 1], x + c, y + r)) continue;
                px(x + c, y + r, fg);
            }
    }
    return x + w;
}

int gfx_text(int x, int y, const char *s, u32 fg, u32 bg)
{
    while (*s) {
        u32 cp = u8_next(&s);
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
