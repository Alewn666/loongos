/* /bin/bad —— 反面教材：演示两道防线
 * ① 用 syscall 递内核地址 → 内核指针校验拒绝
 * ② 不走 syscall 直接读内核内存 → 权限故障 → 内核把它杀掉 */
#include "user.h"

void _start(void)
{
    u_puts("bad 程序启动：专干坏事\n");

    u_puts("坏事①：拿内核地址 0x40080000 调 sys_write\n");
    long r = sys_call(0, (long)0x40080000UL, 16);
    if (r == -1)
        u_puts("→ 内核拒了，返回 -1（预期）\n");
    else
        u_puts("?? 内核没拒（不应该！）\n");
    (void)r;

    u_puts("坏事②：不走 syscall，直接读内核内存\n");
    volatile u64 v = *(volatile u64 *)0x40080000UL;   /* EL0 读内核 → 权限故障 → 被杀 */
    (void)v;

    u_puts("?? 活着（不应该）\n");
    sys_exit(1);
}
