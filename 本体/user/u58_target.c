/* 第五十八轮 58.0-A：execve 的目标程序（被 u58_exec 加载替换）
 * -nostdlib EL0。证明 execve 成功：打印独立标记 + 自己的 pid（应与 execve 前相同），
 * 打印 argc/argv[0]（验证参数区重建），然后 sys_exit 体面退出。
 */
#include "user.h"

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
{ u_puts("[R58EX] "); u_puts(k); u_puts("="); pdec(v); pc('\n'); }

int main(void)
{
    u_puts("[R58EX] replaced=1\n");
    kv("pid", u_getpid());
    kv("argc", u_argc());
    if (u_argc() > 0) {
        u_puts("[R58EX] argv0=");
        u_puts(u_argv(0));
        pc('\n');
    }
    if (u_argc() > 1) {
        u_puts("[R58EX] argv1=");
        u_puts(u_argv(1));
        pc('\n');
    }
    kv("target_exit", 1);
    sys_exit(0);
    for (;;) {}
}
