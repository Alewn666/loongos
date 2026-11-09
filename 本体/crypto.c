#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS SHA-256（FIPS 180-4 标准算法，纯 C 实现）
 * 栈占用：sha256_block 内 w[64]=256B + 尾块 tail[128] → 合计 ~400B，
 * 内核栈由 linker.ld 在 .bss 里留 32KB（. += 32 * 1024），余量充足。 */

#include "crypto.h"

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* 压缩一个 512 位块 */
static void sha256_block(u32 h[8], const u8 *p)
{
    u32 w[64];
    u32 a = h[0], b = h[1], c = h[2], d = h[3];
    u32 e = h[4], f = h[5], g = h[6], hh = h[7];
    int i;

    for (i = 0; i < 16; i++) w[i] = be32(p + i * 4);
    for (i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    for (i = 0; i < 64; i++) {
        u32 S1  = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        u32 ch  = (e & f) ^ ((~e) & g);
        u32 t1  = hh + S1 + ch + K[i] + w[i];
        u32 S0  = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2  = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(const u8 *data, u32 len, u8 out[32])
{
    u32 h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    u32 off = 0;
    u8 tail[128];
    u32 rem;

    while (off + 64 <= len) { sha256_block(h, data + off); off += 64; }

    /* 尾块：消息 + 0x80 + 填 0 + 64 位大端长度（位数）
       ⚠️ 填零个数 = 55-rem（不是 56-rem！）：0x80 本身占 1 字节，长度场要正好落在 56..63。
       算错的症状："abc" 这类短消息哈希全错、空串"碰巧"对（长度场被截进块里还越界写 4 字节）。 */
    rem = len - off;
    for (u32 i = 0; i < rem; i++) tail[i] = data[off + i];
    tail[rem] = 0x80;
    u32 padzero = (rem < 56) ? (55 - rem) : (119 - rem);   /* 0x80 之后的 0 字节数 */
    for (u32 i = 0; i < padzero; i++) tail[rem + 1 + i] = 0;
    u64 bits = (u64)len * 8;
    put_be32(tail + rem + 1 + padzero, (u32)(bits >> 32));
    put_be32(tail + rem + 1 + padzero + 4, (u32)bits);
    u32 taillen = rem + 1 + padzero + 8;   /* 64 或 128 */

    sha256_block(h, tail);
    if (taillen == 128) sha256_block(h, tail + 64);

    for (int i = 0; i < 32; i++) out[i] = (u8)(h[i >> 2] >> (24 - 8 * (i & 3)));
}

static u32 str_len(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

void sha256_iter(const u8 *salt, u32 saltlen, const char *pw, u32 iter, u8 out[32])
{
    /* 第一趟：SHA256(salt || pw) */
    u8 buf[160];
    u32 plen = str_len(pw);
    if (saltlen > 64) saltlen = 64;
    if (plen > 64) plen = 64;
    for (u32 i = 0; i < saltlen; i++) buf[i] = salt[i];
    for (u32 i = 0; i < plen; i++) buf[saltlen + i] = (u8)pw[i];
    sha256(buf, saltlen + plen, out);

    /* 后续 iter-1 趟：SHA256(上一趟结果 || salt) */
    if (iter < 1) iter = 1;
    for (u32 i = 1; i < iter; i++) {
        for (u32 j = 0; j < 32; j++) buf[j] = out[j];
        for (u32 j = 0; j < saltlen; j++) buf[32 + j] = salt[j];
        sha256(buf, 32 + saltlen, out);
    }
}

void to_hex(const u8 *in, u32 n, char *out)
{
    static const char hd[] = "0123456789abcdef";
    for (u32 i = 0; i < n; i++) {
        out[i * 2]     = hd[in[i] >> 4];
        out[i * 2 + 1] = hd[in[i] & 15];
    }
    out[n * 2] = 0;
}
