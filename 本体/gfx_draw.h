/* LoongOS 桌面路线 P1 · 绘图实现层接口（gfx_draw.h）
 *
 * 分层：gfx.h 是「零状态纯函数层」（宿主可单测），本头文件声明 gfx.c 里
 * 真正往帧缓冲画像素的「有状态层」。两者刻意分开，改渲染器不影响纯函数单测。
 *
 * 依赖方向：gfx.c 只依赖 framebuffer 参数 + kernel.c 导出的 fb_glyph_at()
 * （字库只在 kernel.c 有一份，避免 font_cjk.h 的 885 个字形被复制 32KB）。
 */
#ifndef LOONGOS_GFX_DRAW_H
#define LOONGOS_GFX_DRAW_H

#include <stdint.h>
#include "gfx.h"

/* 颜色：0x00RRGGBB，与现有 fb_putc 写帧缓冲的格式一致 */
#define GFX_RGB(r, g, b) ((uint32_t)((((uint32_t)(r) & 255u) << 16) | \
                                     (((uint32_t)(g) & 255u) << 8)  | \
                                     ((uint32_t)(b) & 255u)))

#define GFX_CLIP_DEPTH 8

/* 初始化：只登记参数，不写内存（可在 MMU 开启前调用） */
void gfx_init(void *fb, int w, int h, int stride);
int  gfx_ready(void);
int  gfx_width(void);
int  gfx_height(void);

/* 屏幕所有权：桌面接管期间 kernel.c 的 uart_putc 不再把日志镜像到帧缓冲，
   否则串口日志的文字滚屏会把桌面画面冲掉。串口本身照常输出。 */
int  gfx_screen_owned(void);
void gfx_take_screen(void);
void gfx_release_screen(void);

/* 图元（全部先过裁剪栈 + 屏幕裁剪） */
void gfx_fill(struct gfx_rect r, uint32_t rgb);
void gfx_rect_draw(struct gfx_rect r, uint32_t rgb, int thickness);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t rgb);

/* P3.3 文本插入光标（caret）：架构中立的绘制 helper，ARM 图形终端与 x86
   键盘回显窗口共用。闪烁「相位」由各自架构的时钟驱动（ARM 用 cntpct_el0，
   x86 用 PIT g_ticks），这里只负责把相位画成一个 8x16 的方块。
   on_rgb/off_rgb 由调用方给，因为两边输入行底色不同。放在 gfx_fill 声明
   之后，避免 C 的「先声明后使用」。 */
static inline void gfx_draw_caret(int x, int y, int on,
                                  uint32_t on_rgb, uint32_t off_rgb)
{
    gfx_fill((struct gfx_rect){x, y, 8, 16}, on ? on_rgb : off_rgb);
}

/* 文字：ASCII 8x16 + UTF-8 中文 16x16（码点绘制复用 kernel.c 的字库）。
   bg 为 0xFFFFFFFF 时表示透明（只画前景）。返回绘制后的 x 坐标。 */
int  gfx_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);
int  gfx_text_w(const char *s);

void gfx_clip_push(struct gfx_rect r);
void gfx_clip_pop(void);

/* 脏矩形：gfx_damage 登记重绘区，gfx_present 逐区回调 redraw。
   表满自动退化为整屏（gfx.h 里的纯函数保证不越界、不分配内存）。 */
void gfx_set_redraw(void (*fn)(struct gfx_rect r));
void gfx_damage(struct gfx_rect r);
void gfx_present(void);

#endif /* LOONGOS_GFX_DRAW_H */
