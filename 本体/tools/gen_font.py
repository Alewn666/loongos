#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""增量补齐 16x16 点阵字库 font_cjk.h（勿全量重生成，会顶穿 rodata 预算）。

背景：原生成器遗失；DroidSansFallbackFull.ttf 在沙箱已不存在。字库现有 885
字形来自 Droid，风格字节不可动。新增中文 App 时，只用本脚本把**新增显示文本
里缺的字**补进去（用 Noto Sans CJK SC 渲染），原字形字节保持不变。

⚠️ 不要把源码里所有非 ASCII 字一次性补齐：源码（含注释/文档字符串）有 1200+
码点，全补会 +70KB rodata 直接顶穿内核上界守卫（实测 FAIL）。只补确实要显示
在帧缓冲上的字。

用法（在 本体/ 目录）：
  python3 tools/gen_font.py --check            # 检查 EXTRA 里的字是否都已入库
  python3 tools/gen_font.py                    # 把 EXTRA 中缺的字补进字库
"""
import os
import re
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "font_cjk.h")
FONT_PATH = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_INDEX = 2                     # Noto Sans CJK SC
SIZE = 16
THRESHOLD = 128
Y_OFF = -2

# 需要新增显示的字。改中文 App 字符串后，把新出现的字加到这里再跑一次。
EXTRA = "五今日月蛇贪栏菜题"


def parse():
    txt = open(OUT, "r", encoding="utf-8").read()
    d = {}
    for m in re.finditer(r"\{ (0x[0-9A-Fa-f]+), \{ ([^}]*) \} \}", txt):
        cp = int(m.group(1), 16)
        vals = [int(x, 16) for x in m.group(2).replace("\n", "").split(",") if x.strip()]
        if len(vals) == 32:
            d[cp] = vals
    return d


def render(cp, font):
    img = Image.new("L", (16, 16), 0)
    ImageDraw.Draw(img).text((0, Y_OFF), chr(cp), font=font, fill=255)
    px = img.load()
    bmp = []
    for y in range(16):
        hi = lo = 0
        for x in range(8):
            if px[x, y] >= THRESHOLD:     hi |= (1 << (7 - x))
            if px[8 + x, y] >= THRESHOLD: lo |= (1 << (7 - x))
        bmp += [hi, lo]
    return bmp


def write(d):
    allcp = sorted(d)
    lines = [
        "/* 自动生成：CJK 16x16 点阵字库，取自 DroidSansFallback.ttf；",
        "   2026-11-18 增量补字（新字取自 Noto Sans CJK SC，原字形字节不变）。",
        "   收录内核源码里实际用到的 %d 个非 ASCII 字符。" % len(allcp),
        "   生成器：tools/gen_font.py（改了中文字符串要重跑它再重编译）。别手改。 */",
        "#ifndef LOONGOS_FONTCJK_H", "#define LOONGOS_FONTCJK_H", "",
        "#define CJK_W 16", "#define CJK_H 16", "#define CJK_COUNT %d" % len(allcp), "",
        "typedef struct { unsigned int cp; unsigned char bmp[32]; } cjk_glyph;", "",
        "static const cjk_glyph font_cjk[CJK_COUNT] = {",
    ]
    for cp in allcp:
        lines.append("    { 0x%04X, { %s } }," % (cp, ", ".join("0x%02X" % v for v in d[cp])))
    lines += ["};", "", "#endif /* LOONGOS_FONTCJK_H */", ""]
    open(OUT, "w", encoding="utf-8").write("\n".join(lines))
    print("[gen] 已写 %s：%d 字形" % (OUT, len(allcp)))


def main():
    d = parse()
    missing = sorted(set(ord(c) for c in EXTRA) - set(d))
    if "--check" in sys.argv:
        print("[check] 字库=%d 字，EXTRA 缺失=%d: %s" %
              (len(d), len(missing), " ".join("U+%04X(%s)" % (c, chr(c)) for c in missing)))
        sys.exit(1 if missing else 0)
    if not missing:
        print("[gen] EXTRA 全部已入库，无需改动（%d 字形）" % len(d))
        return
    font = ImageFont.truetype(FONT_PATH, SIZE, index=FONT_INDEX)
    for cp in missing:
        d[cp] = render(cp, font)
        print("       + U+%04X %s" % (cp, chr(cp)))
    write(d)


if __name__ == "__main__":
    main()
