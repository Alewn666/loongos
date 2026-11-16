/* 第五十五轮用户态测试/演示程序的共用小工具（无 libc，纯 SVC 输出）
 * 只依赖 user.h 的 u_puts / sys_call；不放进内核镜像，随 ELF 注入内存盘。
 */
#ifndef LOONGOS_U55_UTIL_H
#define LOONGOS_U55_UTIL_H
#include "user.h"     /* u_puts / sys_call3 / u_write / 以及第五十五轮 syscall 封装 */

static inline long u_sl(const char *s) { long n = 0; while (s[n]) n++; return n; }

/* 诊断输出一律走老接口 write(用户指针,len)（x0 落在用户区）——
   不依赖 fd 1，dup2 把 stdout 重定向到 pipe 时也不会把诊断吞掉。 */
static inline void u_putc(char c) { char b[2]; b[0] = c; b[1] = 0; u_puts(b); }

static inline void u_dec(long v)
{
    char d[24]; int k = 0;
    if (v < 0) { u_putc('-'); v = -v; }
    if (!v) d[k++] = '0';
    while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) u_putc(d[--k]);
}

static inline void u_hex(unsigned long v)
{
    static const char hx[] = "0123456789abcdef";
    char d[20]; int k = 0;
    if (!v) d[k++] = '0';
    while (v) { d[k++] = hx[v & 15]; v >>= 4; }
    while (k) u_putc(d[--k]);
}

static inline int u_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static inline int u_has(const char *hay, const char *needle)
{
    long nl = u_sl(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++) {
        long i = 0;
        while (i < nl && p[i] == needle[i]) i++;
        if (i == nl) return 1;
    }
    return 0;
}

/* 结构化输出：[R55] key=<十进制>  /  [R55] key=<字符串> */
static inline void kv(const char *key, long val)
{ u_puts("[R55] "); u_puts(key); u_puts("="); u_dec(val); u_puts("\n"); }
static inline void ks(const char *key, const char *val)
{ u_puts("[R55] "); u_puts(key); u_puts("="); u_puts(val); u_puts("\n"); }

/* 计数器：每项打印一行 [R55] chk <name> ok|FAIL */
static inline void chk(int *pass, int *fail, const char *name, int cond)
{
    u_puts("[R55] chk "); u_puts(name); u_puts(cond ? " ok\n" : " FAIL\n");
    if (cond) (*pass)++; else (*fail)++;
}

#endif /* LOONGOS_U55_UTIL_H */
