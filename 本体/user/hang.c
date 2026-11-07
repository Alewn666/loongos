/* /bin/hang —— 死循环程序：演示 30 秒看门狗强杀
 * 特点：进入死循环后**不发任何系统调用** —— 正是 EL0 看门狗的靶子：
 * 内核每个 tick 检查 (tick - el0_enter_tick) > EL0_TIMEOUT_TICKS(300=30s)
 * 就强杀 EL0 程序、撤回内核，shell 继续（整机不再被用户程序挂死）。
 * 用法: run /bin/hang   预期 ~30 秒后看到超时被杀的提示。 */
#include "user.h"

void _start(void)
{
    u_puts("hang 程序启动：死循环，不发任何系统调用\n");
    u_puts("预期 ~30 秒后被内核看门狗强杀，回到 shell\n");
    volatile unsigned long spin = 0;   /* volatile 防优化；故意死循环 */
    for (;;) spin++;
}
