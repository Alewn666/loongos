/* LoongOS 第三十七轮 · base64 纯函数宿主单测（批次 2 文件进出通道）
 *
 * 编译（在本体目录）：
 *   cc -O1 -g -Wall -Wextra -Werror test_b64_host.c -o /tmp/test_b64 && /tmp/test_b64
 *
 * 覆盖：RFC 4648 标准向量、编码/解码往返一致、非法字符拒绝、空白与换行容忍、
 *       解码 cap 边界（写前检查，绝不越界）、canary 哨兵、非法参数。
 * 与 test_path_host.c 同一纪律：缓冲尾部填 0xA5 哨兵，验完逐字节核对未被踩坏。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "b64.h"

static int ok = 0, fail = 0;
#define CHECK(c, n) do { if (c) { ok++; } else { fail++; printf("  [FAIL] %s (line %d)\n", n, __LINE__); } } while (0)

#define GUARD 16
static int canary_clean(const unsigned char *buf, int cap)
{
    for (int i = cap; i < cap + GUARD; i++)
        if (buf[i] != 0xA5) return 0;
    return 1;
}

/* RFC 4648 标准向量 */
static void rfc_vectors(void)
{
    struct { const char *in; const char *out; } v[] = {
        {"",      ""},
        {"f",     "Zg=="},
        {"fo",    "Zm8="},
        {"foo",   "Zm9v"},
        {"foob",  "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar","Zm9vYmFy"},
    };
    int n = (int)(sizeof(v) / sizeof(v[0]));
    for (int i = 0; i < n; i++) {
        char enc[256];
        int e = b64_encode((const unsigned char *)v[i].in, (int)strlen(v[i].in), enc);
        CHECK(e == (int)strlen(v[i].out), "enc len");
        CHECK(strcmp(enc, v[i].out) == 0, v[i].in);
        unsigned char dec[256 + GUARD];
        memset(dec, 0xA5, sizeof dec);
        int d = b64_decode(v[i].out, dec, 256);
        CHECK(d == (int)strlen(v[i].in), "dec len");
        if (d > 0) CHECK(memcmp(dec, v[i].in, d) == 0, "content");
        CHECK(canary_clean(dec, 256), "rfc canary");
    }
}

/* 往返：多样长度（含 3 的倍数边界）编解码一致 */
static void roundtrip(void)
{
    static unsigned char data[200];
    int seeds[] = {0, 1, 2, 3, 7, 15, 16, 17, 63, 64, 65, 100, 199};
    int ns = (int)(sizeof(seeds) / sizeof(seeds[0]));
    for (int s = 0; s < ns; s++) {
        int len = seeds[s];
        for (int i = 0; i < len; i++) data[i] = (unsigned char)((i * 31 + s * 7) & 0xFF);
        char enc[512];
        int e = b64_encode(data, len, enc);
        CHECK((len + 2) / 3 * 4 == e, "enc len");
        unsigned char dec[256 + GUARD];
        memset(dec, 0xA5, sizeof dec);
        int d = b64_decode(enc, dec, 256);
        CHECK(d == len, "dec len");
        if (d > 0) CHECK(memcmp(dec, data, len) == 0, "content match");
        CHECK(canary_clean(dec, 256), "roundtrip canary");
    }
}

/* 非法字符拒绝 + 空白容忍（空白是「忽略」不是「截断」） */
static void rejects_garbage(void)
{
    unsigned char out[256];
    CHECK(b64_decode("Zm9v!", out, 256) == -1, "bomber !");
    CHECK(b64_decode("Zm9v\x01v", out, 256) == -1, "embedded ctrl");
    /* 之前断言写成 2/3 是错的：空白被忽略后 "Zm 9v" 仍是 "foo" 共 3 字节；
       "Zm9v\nYg==" 是 "foob" 共 4 字节。实现正确，断言修正。 */
    CHECK(b64_decode("Zm 9v", out, 256) == 3, "spaces tolerated");
    CHECK(memcmp(out, "foo", 3) == 0, "spaces content");
    CHECK(b64_decode("Zm9v\nYg==", out, 256) == 4, "newline tolerated");
    CHECK(memcmp(out, "foob", 4) == 0, "newline content");
}

/* cap 保护：本次补上的关键安全性质 */
static void cap_protection(void)
{
    unsigned char b[6 + GUARD];

    /* cap 恰好等于结果长度：成功且哨兵干净 */
    memset(b, 0xA5, sizeof b);
    CHECK(b64_decode("Zm9vYmFy", b, 6) == 6, "exact cap ok");
    CHECK(memcmp(b, "foobar", 6) == 0, "exact cap content");
    CHECK(canary_clean(b, 6), "exact cap canary");

    /* cap 比结果小 1：必须在第 6 次写之前返回 -1，b[5] 之后一个字节都不能动 */
    memset(b, 0xA5, sizeof b);
    CHECK(b64_decode("Zm9vYmFy", b, 5) == -1, "short cap rejected");
    CHECK(canary_clean(b, 5), "short cap canary");

    /* cap = 0 / 负数 / NULL 参数 */
    CHECK(b64_decode("", b, 0) == 0, "cap 0 empty ok");
    CHECK(b64_decode("Zg==", b, 0) == -1, "cap 0 rejected");
    CHECK(b64_decode("Zg==", b, -1) == -1, "negative cap rejected");
    CHECK(b64_decode(0, b, 6) == -1, "null in rejected");
    CHECK(b64_decode("Zg==", 0, 6) == -1, "null out rejected");
}

/* 边界：300 字节完整编解码，输出不越界 */
static void boundary(void)
{
    unsigned char in[300];
    for (int i = 0; i < 300; i++) in[i] = (unsigned char)i;
    char enc[401];                    /* (300+2)/3*4 = 400 + NUL */
    int e = b64_encode(in, 300, enc);
    CHECK(e == 400, "300-byte encode len");
    CHECK(enc[400] == 0, "encode NUL terminator");
    unsigned char dec[300 + GUARD];
    memset(dec, 0xA5, sizeof dec);
    int d = b64_decode(enc, dec, 300);
    CHECK(d == 300, "300-byte decode len");
    CHECK(memcmp(dec, in, 300) == 0, "300-byte content");
    CHECK(canary_clean(dec, 300), "300-byte canary");
}

int main(void)
{
    rfc_vectors();
    roundtrip();
    rejects_garbage();
    cap_protection();
    boundary();
    printf("\n%d/%d 通过\n", ok, ok + fail);
    return fail ? 1 : 0;
}
