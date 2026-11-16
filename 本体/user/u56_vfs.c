/* 第五十六轮用户态 VFS 验证程序（-nostdlib EL0，经 b64 注入 /home）
 * 走 SYS_vfsread/SYS_vfsinfo 读 /proc /dev 伪文件，输出 [R56] key=value
 * 供 test_round56.py 独立核对；同时验证 /etc/hostname 老路径不受 VFS 影响。
 */
#include "user.h"

static long sl(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void pc(char c) { char b[2]; b[0] = c; b[1] = 0; u_puts(b); }
static void pdec(long v)
{
    char d[24]; int k = 0;
    if (v < 0) { pc('-'); v = -v; }
    if (!v) d[k++] = '0';
    while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) pc(d[--k]);
}
static void kv(const char *k, long v)
{ u_puts("[R56] "); u_puts(k); u_puts("="); pdec(v); pc('\n'); }
static void ks(const char *k, const char *v)
{ u_puts("[R56] "); u_puts(k); u_puts("="); u_puts(v); pc('\n'); }
static int has(const char *h, const char *n)
{
    long nl = sl(n);
    for (const char *p = h; *p; p++) {
        long i = 0; while (i < nl && p[i] == n[i]) i++;
        if (i == nl) return 1;
    }
    return 0;
}
/* 提取 "MemTotal:" 后的第一个整数（简单扫描） */
static long numafter(const char *buf, const char *key)
{
    long kl = sl(key);
    for (const char *p = buf; *p; p++) {
        long i = 0; while (i < kl && p[i] == key[i]) i++;
        if (i != kl) continue;
        const char *q = p + kl;
        while (*q == ' ') q++;
        long v = 0; int any = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; any = 1; }
        if (any) return v;
    }
    return -1;
}

int main(void)
{
    char b[512];
    long p, n;
    int pass = 0, fail = 0;

    /* 1) 挂载点数量（开机应挂了 /proc /dev = 2） */
    n = u_vfsinfo();
    kv("vfsinfo", n);
    if (n >= 2) pass++; else fail++;

    /* 2) /proc/meminfo：读成功 + 含 MemTotal/MemFree/kB + 数值自洽 */
    b[0] = 0;
    n = u_vfsread("/proc/meminfo", b, sizeof b - 1);
    b[n > 0 ? n : 0] = 0;
    kv("meminfo_rc", n);
    if (n > 0 && has(b, "MemTotal:") && has(b, "MemFree:") && has(b, "kB")) pass++;
    else fail++;
    long tot = numafter(b, "MemTotal:");
    long fre = numafter(b, "MemFree:");
    kv("meminfo_total_kb", tot);
    kv("meminfo_free_kb", fre);
    if (tot > 0 && fre >= 0 && fre <= tot) pass++; else fail++;

    /* 3) /proc/version / uptime / cpuinfo / mounts */
    b[0] = 0;
    n = u_vfsread("/proc/version", b, sizeof b - 1); b[n > 0 ? n : 0] = 0;
    ks("version", b);
    if (n > 0 && has(b, "LoongOS")) pass++; else fail++;

    b[0] = 0;
    n = u_vfsread("/proc/uptime", b, sizeof b - 1); b[n > 0 ? n : 0] = 0;
    kv("uptime_rc", n);
    if (n > 0 && has(b, ".")) pass++; else fail++;

    b[0] = 0;
    n = u_vfsread("/proc/cpuinfo", b, sizeof b - 1); b[n > 0 ? n : 0] = 0;
    if (n > 0 && has(b, "aarch64")) pass++; else fail++;

    b[0] = 0;
    n = u_vfsread("/proc/mounts", b, sizeof b - 1); b[n > 0 ? n : 0] = 0;
    kv("mounts_rc", n);
    if (n > 0 && has(b, "/proc") && has(b, "/dev") && has(b, "procfs")) pass++;
    else fail++;

    /* 4) devfs：/dev/zero 全零、/dev/null 读 0 */
    for (p = 0; p < 32; p++) b[p] = 0xAA;
    n = u_vfsread("/dev/zero", b, 32);
    kv("zero_rc", n);
    int zero = (n == 32);
    for (p = 0; p < 32; p++) if (b[p] != 0) zero = 0;
    if (zero) pass++; else fail++;

    n = u_vfsread("/dev/null", b, 32);
    kv("null_rc", n);
    if (n == 0) pass++; else fail++;

    /* 5) 负例：不存在的 proc 文件 / 未挂载前缀 → -1 */
    kv("proc_nope_rc", u_vfsread("/proc/nope", b, sizeof b - 1));
    if (u_vfsread("/proc/nope", b, sizeof b - 1) == -1) pass++; else fail++;
    kv("unmounted_rc", u_vfsread("/etc/hostname", b, sizeof b - 1));
    if (u_vfsread("/etc/hostname", b, sizeof b - 1) == -1) pass++; else fail++;

    /* 6) 老路径仍正常：open/read /etc/hostname（走 fs_*，不经 VFS） */
    p = u_open("/etc/hostname");
    kv("old_open_fd", p);
    if (p >= 3) {
        b[0] = 0;
        n = u_read(p, b, 31); b[n > 0 ? n : 0] = 0;
        kv("old_hostname_len", n);
        ks("old_hostname", b);
        if (has(b, "myos")) pass++; else fail++;
        u_close(p);
    } else fail++;

    kv("pass", pass);
    kv("fail", fail);
    u_puts("[R56] done\n");
    sys_exit(fail ? 1 : 0);   /* 体面退出：避免 main return 越界触发 EL0 fault */
}
