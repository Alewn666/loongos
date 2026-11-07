/* /bin/cat —— 第一个用 POSIX 文件接口的用户程序
 * 演示：open / read / write / close 全程跑在 EL0，只有 SVC 能碰内核。
 * 用法（shell）：run /bin/cat            → 读 /home/readme.txt
 *                run /bin/cat            （参数传递尚未实现，路径写死）
 */
#include "user.h"

static char buf[512];

void _start(void)
{
    const char *path = "/home/readme.txt";
    u_puts("[cat] 打开 ");
    u_puts(path);
    u_puts("\n");

    long fd = u_open(path);
    if (fd < 0) {
        u_puts("[cat] 打不开，exit(1)\n");
        sys_exit(1);
    }

    long total = 0;
    for (;;) {
        long n = u_read(fd, buf, (long)sizeof buf);
        if (n <= 0) break;
        u_write(1, buf, n);       /* fd=1 → 串口 */
        total += n;
    }
    u_close(fd);

    u_puts("\n[cat] 读完，共 ");
    {   /* 小整数转十进制（没有 libc） */
        char d[20];
        int k = 0;
        long v = total;
        if (!v) d[k++] = '0';
        while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
        char out[20];
        int m = 0;
        while (k) out[m++] = d[--k];
        out[m] = 0;
        u_puts(out);
    }
    u_puts(" 字节，exit(0)\n");
    sys_exit(0);
}
