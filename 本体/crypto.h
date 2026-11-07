/* LoongOS 阶段5 - 密码学小模块：SHA-256 + 迭代哈希（口令存储用）
 * 纯 C 零依赖；以后做签名/随机数也从这里长。 */

#ifndef LOONGOS_CRYPTO_H
#define LOONGOS_CRYPTO_H

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned char  u8;

/* SHA-256：data[0..len) → out[32] */
void sha256(const u8 *data, u32 len, u8 out[32]);

/* 迭代哈希（PBKDF1 风格，教学版）：
   h = SHA256(salt||pw)，再迭代 iter-1 次 h = SHA256(h||salt)。
   比起单次哈希，迭代让暴力猜口令慢 iter 倍。 */
void sha256_iter(const u8 *salt, u32 saltlen, const char *pw, u32 iter, u8 out[32]);

/* hex 工具：out 必须有 2*n+1 空间（结尾 NUL） */
void to_hex(const u8 *in, u32 n, char *out);

#endif
