/* b64.h —— 纯函数 base64 编解码（无状态，可宿主单测，与 path.h 同一定位）
 *
 * 为什么存在（第三十七轮，批次 2「文件进出通道」）：
 *   LoongOS 已有 cat/write/cp 读写任意字节文件，但文件「进出系统」缺最后一环：
 *   外部文件（图片/文本/小脚本）无法进系统，系统内的文件也无法导出。
 *   没有 U 盘驱动（USB 只做 HID 键鼠）、没有应用层文件传输协议（TCP 只有协议栈），
 *   最稳妥的通用通道是把二进制文件编成 base64 纯文本，经串口 / 图形终端粘贴进出。
 *
 *   b64out <路径>  → 读文件，按 76 字符折行打印 base64（复制即可带走）
 *   b64in  <路径>  → 粘贴 base64 文本（每行<=76 字符，空行或 '.' 结束），解码写回文件
 *
 * 类型：用 unsigned char，不依赖内核 u8 别名；freestanding 与宿主 gcc 均可编译。
 * 不引入堆、不依赖任何内核符号，便于宿主单测 1:1 覆盖真代码。
 */
#ifndef LOONGOS_B64_H
#define LOONGOS_B64_H

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 编码 in[0..len) → out（out 需 >= (len+2)/3*4 + 1 字节）。返回写出长度（不含 NUL）。
   不添加换行；长度非 3 倍数时尾部补 '='。 */
static int b64_encode(const unsigned char *in, int len, char *out)
{
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < len) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned)in[i + 2];
        int rem = len - i;
        out[o++] = b64_alphabet[(v >> 18) & 63];
        out[o++] = b64_alphabet[(v >> 12) & 63];
        if (rem >= 2) out[o++] = b64_alphabet[(v >> 6) & 63];
        if (rem >= 3) out[o++] = b64_alphabet[v & 63];
    }
    int pads = (3 - (len % 3)) % 3;
    for (int i = 0; i < pads; i++) out[o++] = '=';
    out[o] = 0;
    return o;
}

/* 单字符 → 0..63；'-1' 表示非字母（含 '=' 与空白）。 */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 解码 in → out（out 可写 cap 字节）。忽略空白与 '='；
   遇非法字符、输出容量不足或参数非法一律返回 -1，**且绝不越界写**。
   返回解码字节数（>=0）。不足一字节的余位自动丢弃。
   cap 是第三十七轮补上的：内核态调用方（b64in）把解码结果写进固定静态缓冲，
   没有 cap 就是 H1/H2/H3 同款「无边界写」，批次 1 刚修完不能在这里再引入。 */
static int b64_decode(const char *in, unsigned char *out, int cap)
{
    if (!in || !out || cap < 0) return -1;
    int o = 0;
    unsigned acc = 0; int bits = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') continue;
        int v = b64_val(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;   /* 写之前检查：宁可失败也不踩出缓冲 */
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

#endif /* LOONGOS_B64_H */
