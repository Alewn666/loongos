/* crypto.c 的主机 KAT（Known Answer Test）：SHA-256 标准测试向量 + 迭代哈希性质 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"

static int fails = 0, total = 0;
static void ok(int cond, const char *name)
{
    total++;
    if (cond) printf("  [%02d] PASS %s\n", total, name);
    else { printf("  [%02d] FAIL %s\n", total, name); fails++; }
}

int main(void)
{
    printf("== crypto.c host KAT ==\n");
    u8 h[32];
    char hex[65];

    /* SHA-256("abc") — FIPS 180-4 vector */
    sha256((const u8 *)"abc", 3, h);
    to_hex(h, 32, hex);
    ok(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
       "sha256(\"abc\") FIPS vector");

    /* SHA-256("") */
    sha256((const u8 *)"", 0, h);
    to_hex(h, 32, hex);
    ok(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
       "sha256(\"\") empty string");

    /* SHA-256 56 字节消息 = 跨两个 512 位块的经典向量 */
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256((const u8 *)m2, (u32)strlen(m2), h);
    to_hex(h, 32, hex);
    ok(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
       "sha256 two-block 56B vector");

    /* to_hex 往返 */
    u8 three[3] = { 0x00, 0x0f, 0xa5 };
    to_hex(three, 3, hex);
    ok(strcmp(hex, "000fa5") == 0, "to_hex 00/0f/a5");

    /* sha256_iter 性质：(1)同参稳定 (2)改口令变 (3)改盐变 (4)改迭代数变 */
    u8 h1[32], h2[32], h3[32];
    sha256_iter((const u8 *)"NaCl", 4, "correct horse", 2000, h1);
    sha256_iter((const u8 *)"NaCl", 4, "correct horse", 2000, h2);
    ok(memcmp(h1, h2, 32) == 0, "sha256_iter deterministic");
    sha256_iter((const u8 *)"NaCl", 4, "correct horse2", 2000, h3);
    ok(memcmp(h1, h3, 32) != 0, "different password -> different hash");
    sha256_iter((const u8 *)"KCl", 3, "correct horse", 2000, h3);
    ok(memcmp(h1, h3, 32) != 0, "different salt -> different hash");
    sha256_iter((const u8 *)"NaCl", 4, "correct horse", 1999, h3);
    ok(memcmp(h1, h3, 32) != 0, "different iter -> different hash");

    /* 哈希里不应出现明文的 hex 编码（'root2026' 等）——防"伪哈希" */
    sha256_iter((const u8 *)"LoongSlt", 8, "loong2026", 2000, h1);
    to_hex(h1, 32, hex);
    ok(strstr(hex, "6c6f6f6e6732303236") == NULL, "hash has no plaintext 'loong2026' hex");

    /* ---- 第五十四轮：常量时间比较 ct_memeq（演进路线 3.3）---- */
    const char *A = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char B[65];
    strcpy(B, A);
    ok(ct_memeq(A, B, 64) == 1, "ct_memeq 等长相等返回 1");
    ok(ct_memeq(A, A, 0) == 1, "ct_memeq n=0 返回 1");
    /* 每一个差异位置都必须被检出（覆盖「只在头几字节比较」的偷懒实现） */
    int all_diff = 1;
    for (int pos = 0; pos < 64; pos++) {
        strcpy(B, A);
        B[pos] = (char)(B[pos] == 'x' ? 'y' : 'x');
        if (ct_memeq(A, B, 64) != 0) { all_diff = 0; break; }
    }
    ok(all_diff, "ct_memeq 全部 64 个差异位置逐一可检出");
    /* 多字节差异 / 长短相同但内容全异 */
    ok(ct_memeq("aaaaaaaa", "aaaaaaab", 8) == 0, "ct_memeq 末字节差异可检出");
    ok(ct_memeq("aaaaaaaa", "bbbbbbbb", 8) == 0, "ct_memeq 全异可检出");
    /* 不等长语义：只比较给定长度，前 n 字节相同应返回相等 */
    ok(ct_memeq("abcdefgh", "abcdXXXX", 4) == 1, "ct_memeq 仅比较前 n 字节");
    ok(ct_memeq("abcdefgh", "abcdXXXX", 8) == 0, "ct_memeq 第 5 字节起差异可检出");

    printf("== %d/%d passed ==\n", total - fails, total);
    return fails ? 1 : 0;
}
