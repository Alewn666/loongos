/* LoongOS x86_64 · 字库桥
 * 直接复用 ARM 版的 font8x16.h / font_cjk.h（纯 C 数据，架构无关），
 * 给 gfx.c 提供 fb_query_glyph()。字库只有这一份实例（CJK 885 字形 ≈ 32KB）。
 * 用 -I../本体 让编译器找到这两个头文件。
 */
#include <stdint.h>
#include "font8x16.h"
#include "font_cjk.h"

typedef uint8_t  u8;
typedef uint32_t u32;

/* font_cjk 按 cp 升序，二分查找 */
static const cjk_glyph *cjk_find(u32 cp)
{
    int lo = 0, hi = CJK_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u32 c = font_cjk[mid].cp;
        if (c == cp) return &font_cjk[mid];
        if (c < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* gfx.c 依赖：返回 0=找到；row_bytes: ASCII=1, CJK=2 */
int fb_query_glyph(u32 cp, const u8 **bmp, int *w, int *h, int *row_bytes)
{
    if (cp < 128) {
        unsigned char ch = (unsigned char)cp;
        if (ch < FONT_FIRST || ch >= FONT_FIRST + FONT_COUNT) ch = '?';
        *bmp = font8x16[ch - FONT_FIRST];
        *w = FONT_W; *h = FONT_H; *row_bytes = 1;
        return 0;
    }
    const cjk_glyph *g = cjk_find(cp);
    if (!g) return -1;
    *bmp = g->bmp;
    *w = CJK_W; *h = CJK_H; *row_bytes = 2;
    return 0;
}
