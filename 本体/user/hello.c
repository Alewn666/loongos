/* /bin/hello —— 阶段5② 第一个从文件系统加载、跑在 EL0 的用户程序 */
#include "user.h"

void _start(void)
{
    u_puts("你好！我是 /bin/hello，正跑在 EL0 用户态\n");
    u_puts("我住在 0x401C0000 起的用户区，摸不到内核的内存\n");
    u_puts("跟内核说话只能走 syscall。下面调 exit(0) 体面退出\n");
    sys_exit(0);
}
