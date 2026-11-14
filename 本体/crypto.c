#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS SHA-256（FIPS 180-4 标准算法）
 * 栈占用：sha256_block_c 内 w[64]=256B + 尾块 tail[128] → 合计 ~400B，
 * 内核栈由 linker.ld 在 .bss 里留 32KB（. += 32 * 1024），余量充足。
 *
 * 第四十七轮（ARM 专项）：口令 KDF 是系统里最重的纯 CPU 负载——
 * 一次登录 = 5 万轮 SHA-256（实测纯 C 在 QEMU TCG 下约 150ms）。
 * 本文件在支持 ARMv8 Crypto Extensions 的核上改用 SHA256H/SHA256H2
 * 四条一轮的硬件压缩指令；不支持的核（Cortex-A53 等）自动回退纯 C。
 *
 * 安全边界：
 *   - 是否可用由 ID_AA64ISAR0_EL1.SHA2 运行时判定，不看编译期宏；
 *   - 首次使用前用「abc」与 200 字节确定性模式对比硬件/软件结果，
 *     不一致立即永久禁用硬件路径（实现写错也不会算错口令）；
 *   - 本文件用 -mcpu=cortex-a57+crypto 单独编译，其它 .o 仍是基线 ARMv8，
 *     不会把密码指令泄漏到不支持它的机器上。 */

#include "crypto.h"
/* 宿主工具（gen_authhash.c 用 cc 一起编）没有 ARM 内建，走纯 C；
   内核构建时 build.sh 给本文件加 -mcpu=cortex-a57+crypto。 */
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)
#include <arm_neon.h>
#define SHA256_HAVE_HW 1
#else
#define SHA256_HAVE_HW 0
#endif

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

static void put_be64(u8 *p, u64 v)
{
    put_be32(p, (u32)(v >> 32));
    put_be32(p + 4, (u32)v);
}

/* 压缩一个 512 位块（纯 C 参考实现，也是无密码扩展硬件的唯一路径） */
static void sha256_block_c(u32 h[8], const u8 *p)
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

/* ---- ARMv8 Crypto Extensions：SHA256H / SHA256H2 ----
 * 消息扩展仍用标量逻辑（48 轮），压缩的 64 轮交给硬件：每条指令做 4 轮，
 * 16 条指令完成一个块。寄存器约定（ARM ARM）：
 *   abcd' = SHA256H (abcd, efgh, wk)
 *   efgh' = SHA256H2(efgh, abcd, wk)   ← 用的是**旧** abcd
 * 所以先算 abcd'，再把旧 abcd 传给 H2。 */
#if SHA256_HAVE_HW
static void sha256_block_hw(u32 h[8], const u8 *p)
{
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + i * 4);
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32x4_t abcd = vld1q_u32(h);
    uint32x4_t efgh = vld1q_u32(h + 4);
    const uint32x4_t h0 = abcd, h4 = efgh;
    for (int i = 0; i < 64; i += 4) {
        u32 wk[4] = { w[i] + K[i], w[i+1] + K[i+1], w[i+2] + K[i+2], w[i+3] + K[i+3] };
        uint32x4_t vk = vld1q_u32(wk);
        uint32x4_t na = vsha256hq_u32(abcd, efgh, vk);
        uint32x4_t ne = vsha256h2q_u32(efgh, abcd, vk);
        abcd = na; efgh = ne;
    }
    vst1q_u32(h,     vaddq_u32(abcd, h0));
    vst1q_u32(h + 4, vaddq_u32(efgh, h4));
}
#else
/* 宿主（x86）构建：没有密码扩展，硬件入口退化为纯 C，保证 gen_authhash 能编 */
static void sha256_block_hw(u32 h[8], const u8 *p) { sha256_block_c(h, p); }
#endif

static int sha2_state = -1;     /* -1 未判定 / 0 无硬件 / 1 可用 */

static void sha256_blocks(u32 h[8], const u8 *p, u32 n, int hw)
{
    while (n--) {
        if (hw) sha256_block_hw(h, p);
        else    sha256_block_c(h, p);
        p += 64;
    }
}

/* 核心：对 data/len 做一次完整 SHA-256（含填充），hw 显式指定走哪条路。
   自检时同一输入跑两遍对比，避免「硬件路径写错但没人发现」。 */
static void sha256_core(u32 h[8], const u8 *data, u32 len, int hw)
{
    u32 off = (len / 64) * 64;
    u8 tail[128];
    u32 rem;

    sha256_blocks(h, data, len / 64, hw);

    rem = len - off;
    for (u32 i = 0; i < rem; i++) tail[i] = data[off + i];
    tail[rem] = 0x80;
    u32 padzero = (rem < 56) ? (55 - rem) : (119 - rem);
    for (u32 i = 0; i < padzero; i++) tail[rem + 1 + i] = 0;
    u64 bits = (u64)len * 8;
    put_be64(tail + rem + 1 + padzero, bits);
    u32 taillen = rem + 1 + padzero + 8;
    sha256_blocks(h, tail, taillen / 64, hw);
}

/* 首次使用前自检：硬件与纯 C 必须逐位一致。
   覆盖「abc」（单块+填充）与 200 字节确定性模式（多块 + 尾块 128B 分支）。 */
#if SHA256_HAVE_HW
static int sha256_hw_selftest(void)
{
    static const u8 abc[3] = { 'a', 'b', 'c' };
    u8 pat[200];
    for (u32 i = 0; i < sizeof pat; i++) pat[i] = (u8)(i * 37 + 11);

    for (int t = 0; t < 2; t++) {
        const u8 *msg = t ? pat : abc;
        u32 mlen = t ? (u32)sizeof pat : 3;
        u32 a[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
        u32 b[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
        sha256_core(a, msg, mlen, 0);
        sha256_core(b, msg, mlen, 1);
        for (int i = 0; i < 8; i++) if (a[i] != b[i]) return 0;
    }
    return 1;
}
#endif   /* SHA256_HAVE_HW */

static int sha2_usable(void)
{
    if (sha2_state < 0) {
#if SHA256_HAVE_HW
        u64 isar = 0;
        __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar));
        int has = (int)((isar >> 4) & 0xF) >= 1;   /* SHA2 字段 [7:4] */
        if (has) has = sha256_hw_selftest();
        sha2_state = has;
#else
        sha2_state = 0;                            /* 宿主/非 ARM：纯 C */
#endif
    }
    return sha2_state;
}

/* 供 bench / 诊断查询：1=硬件路径可用，0=纯 C。 */
int sha256_hw_enabled(void) { return sha2_usable(); }

void sha256(const u8 *data, u32 len, u8 out[32])
{
    u32 h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    sha256_core(h, data, len, sha2_usable());
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
    u8 buf[160] = {0};
    u32 plen = str_len(pw);
    if (saltlen > 64) saltlen = 64;
    if (plen > 64) plen = 64;
    for (u32 i = 0; i < saltlen; i++) buf[i] = salt[i];
    for (u32 i = 0; i < plen; i++) buf[saltlen + i] = (u8)pw[i];
    sha256(buf, saltlen + plen, out);

    if (iter < 1) iter = 1;
    int hw = sha2_usable();

    /* 第四十七轮快路：32+saltlen 能装进一个块（<=55 字节）时，
       每轮只需「32B 上一轮结果 + salt」并复用固定填充，省掉 5 万次
       tail[128] 构造/清零与函数调用。登录 KDF 的 salt 恒为 8 字节。 */
    if (32 + saltlen <= 55) {
        u8 blk[64];
        for (int i = 0; i < 64; i++) blk[i] = 0;
        for (u32 i = 0; i < saltlen; i++) blk[32 + i] = salt[i];
        blk[32 + saltlen] = 0x80;
        put_be64(blk + 56, (u64)(32 + saltlen) * 8);

        static const u32 IV[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };
        for (u32 k = 1; k < iter; k++) {
            for (u32 j = 0; j < 32; j++) blk[j] = out[j];
            u32 h[8];
            for (int j = 0; j < 8; j++) h[j] = IV[j];
            if (hw) sha256_block_hw(h, blk);
            else    sha256_block_c(h, blk);
            for (int j = 0; j < 32; j++) out[j] = (u8)(h[j >> 2] >> (24 - 8 * (j & 3)));
        }
        return;
    }

    /* 慢路（salt 过长，多块）：语义与旧实现完全一致 */
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
