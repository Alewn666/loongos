/* 第五十八轮 58.0-A：完整 execve 验证（调用者）
 * -nostdlib EL0，经 b64 注入 /home，只编不内嵌。
 *
 * 流程：
 *   打印 before_exec / pid_before；
 *   execve("/home/u58tgt", "HELLO")  ← 成功则不返回，目标程序 u58_target 接管：
 *       目标打印 replaced=1、pid（应与 pid_before 相同）、argc/argv0。
 *   若 execve 返回（=失败）：打印 exec_failed 并 sys_exit(1)。
 *   判据：串口出现 replaced=1 且 pid 不变 且 argv0=HELLO 且**无** exec_failed / 无 after 行。
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
{ u_puts("[R58CALL] "); u_puts(k); u_puts("="); pdec(v); pc('\n'); }

int main(void)
{
    kv("pid_before", u_getpid());
    u_puts("[R58CALL] before_exec=1\n");
    long r = u_execve("/home/u58tgt", "HELLO");
    /* 走到这 = execve 失败返回 -1（成功永不返回） */
    kv("exec_ret", r);
    u_puts("[R58CALL] exec_failed=1\n");
    sys_exit(1);
    for (;;) {}
}
