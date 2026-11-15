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

/* 第四十七轮：1 = 正在使用 ARMv8 密码扩展，0 = 纯 C 回退（诊断/基准用） */
int  sha256_hw_enabled(void);

/* 第五十四轮（安全加固，演进路线 3.3）：常量时间内存比较。
 * 全程不提前 return、不对秘密字节做分支，返回 1=相等 / 0=不等。
 * 用于替换 shadow_verify 里「逐字节不等就 return」的口令哈希比对，
 * 消除「前几个字节猜对了就慢一点」的时序侧信道。 */
int  ct_memeq(const void *a, const void *b, u32 n);

#endif
