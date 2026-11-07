/* /bin/args —— 验证 argv 传递：把收到的参数一个个打出来
 * 用法：run /bin/args hello world 42
 */
#include "user.h"

void _start(void)
{
    long argc = u_argc();
    u_puts("[args] argc=");
    char d[20]; int k = 0; long v = argc;
    if (!v) d[k++] = '0';
    while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    char out[20]; int m = 0;
    while (k) out[m++] = d[--k];
    out[m] = 0;
    u_puts(out);
    u_puts("\n");
    for (long i = 0; i < argc; i++) {
        u_puts("[args] argv[");
        {   char n[8]; int t = 0; long x = i; if (!x) n[t++] = '0';
            while (x) { n[t++] = (char)('0' + x % 10); x /= 10; }
            char o2[8]; int m2 = 0; while (t) o2[m2++] = n[--t]; o2[m2] = 0; u_puts(o2); }
        u_puts("] = ");
        u_puts(u_argv(i));
        u_puts("\n");
    }
    sys_exit(0);
}
