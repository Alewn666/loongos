/* 第五十八轮 58.0-A：execve 失败回滚验证
 * -nostdlib EL0。execve 一个不存在的路径，期望返回 -1、原程序继续执行
 * （打印 exec_failed），证明 load_elf_current 阶段一校验失败时用户区未被破坏、
 * 原程序现场完好（失败回滚）。然后 sys_exit 体面退出。
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
{ u_puts("[R58BAD] "); u_puts(k); u_puts("="); pdec(v); pc('\n'); }

int main(void)
{
    kv("pid_before", u_getpid());
    u_puts("[R58BAD] before=1\n");
    long r = u_execve("/home/NO_SUCH_PROGRAM", "X");
    kv("exec_ret", r);                       /* 期望 -1 */
    if (r == -1) u_puts("[R58BAD] rollback_ok=1\n");
    kv("pid_after", u_getpid());             /* 原程序仍在，pid 不变 */
    sys_exit(0);
    for (;;) {}
}
