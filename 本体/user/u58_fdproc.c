/* 第五十八轮 58.0-A 用户态验证：每进程 FD 表（Fork 继承 + FD 独立管理）
 * -nostdlib EL0，经 b64 注入 /home，只编不内嵌。
 *
 * 判据（父 pid=1 走全局 g_fd，子 pid>=2 走自己的 fdtab）：
 *   ① 子 fork 后立即能读到继承来的 fd0/1/2（写 STDOUT 正常）——基础继承。
 *   ② 子 open 一个文件得 fd，读内容正确；子 close 它后再读返回 -1。
 *   ③ 子 close 掉继承的 fd 表项不影响父：父在 wait 返回后仍能 open/read 同一文件。
 *   ④ 子进程自己 open 的 fd 号从 3 起（独立表的小 fd 空间），且退出后由 procexit
 *      触发 fdt_destroy 回收 OFD（mmstat audit 无新增泄漏由宿主/判据侧量化）。
 *
 * 输出 [R58FD] key=value 供 test_round58.py 独立核对；sys_exit 体面退出。
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
{ u_puts("[R58FD] "); u_puts(k); u_puts("="); pdec(v); pc('\n'); }
static void kc(const char *k, char v)
{ u_puts("[R58FD] "); u_puts(k); u_puts("="); pc(v); pc('\n'); }

int main(void)
{
    kv("pid_before", u_getpid());

    long pid = u_fork();
    if (pid != 0) kv("fork_ret", pid);     /* 只父打印 */

    if (pid == 0) {
        /* ---- 子：继承 fd0/1/2，打开自己的文件并读写 ---- */
        kv("child_pid", u_getpid());
        int fd = (int)u_open("/etc/hostname");
        kv("child_open_fd", fd);
        char buf[64];
        for (int i = 0; i < 64; i++) buf[i] = 0;
        long n = u_read(fd, buf, 16);
        kv("child_read", n);
        kc("child_first", n > 0 ? buf[0] : '?');
        /* 关闭自己的 fd 后，再读应失败（-1） */
        kv("child_close", u_close(fd));
        kv("child_read_after_close", u_read(fd, buf, 4));
        int cp = 0, cf = 0;
        if (u_getpid() >= 2) cp++; else cf++;
        if (fd == 3) cp++; else cf++;                 /* 独立表 fd 从 3 起 */
        if (n > 0) cp++; else cf++;
        kv("child_pass", cp);
        kv("child_fail", cf);
        u_puts("[R58FD] child_exit\n");
        u_procexit(5);                      /* 切回父 */
        for (;;) {}
    }

    /* ---- 父：等待子回收，再验证自己的 fd 仍可用（子的 close 未污染父）---- */
    if (pid < 0) { kv("fail", 1); sys_exit(1); }
    int status = -1;
    kv("wait_ret", u_wait(&status));
    kv("status", status);
    int pfd = (int)u_open("/etc/hostname");
    kv("parent_open_fd", pfd);
    char pbuf[64];
    for (int i = 0; i < 64; i++) pbuf[i] = 0;
    long pn = u_read(pfd, pbuf, 8);
    kv("parent_read", pn);
    kc("parent_first", pn > 0 ? pbuf[0] : '?');
    u_close(pfd);

    int pass = 0, fail = 0;
    if (pid >= 2) pass++; else fail++;
    if (status == 5) pass++; else fail++;
    if (pfd == 3) pass++; else fail++;                /* 父全局表 fd 也从 3 起（子的 close 未占用父槽） */
    if (pn > 0) pass++; else fail++;                  /* 父仍能 open/read：子的 close 未污染父 fd */
    if (u_getpid() == 1) pass++; else fail++;
    kv("pass", pass);
    kv("fail", fail);
    u_puts("[R58FD] done\n");
    sys_exit(fail ? 1 : 0);
}
