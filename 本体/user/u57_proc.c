/* 第五十七轮用户态进程模型验证（-nostdlib EL0，经 b64 注入 /home，只编不内嵌）
 *
 * 验证路线 §3.2 的两条核心判据：
 *   ② fork 出的子进程改自己的全局变量，父进程值不变（每进程独立地址空间）；
 *   ③ execve/exit/wait 后进程表无残留（本程序用 procexit + wait 验证回收）。
 *
 * 流程：
 *   g_shared = 0x1111（.data，fork 时按页复制进子的独立物理页）
 *   pid = fork()
 *     == 0  （子）：g_shared = 0x2222；procexit(7)  ← 子改的是自己的拷贝
 *     > 0   （父）：wait(&status) → 拿到 pid 与退出码 7；
 *                  断言自己的 g_shared 仍是 0x1111（未被子污染）
 *   输出 [R57] key=value 供 test_round57.py 独立核对；sys_exit 体面退出。
 */
#include "user.h"

static long sl(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void pc(char c) { char b[2]; b[0] = c; b[1] = 0; u_puts(b); }
static void phex(long v)
{
    const char *H = "0123456789abcdef";
    char d[18]; int k = 0;
    if (!v) d[k++] = '0';
    while (v) { d[k++] = H[v & 0xF]; v >>= 4; }
    pc('0'); pc('x');
    while (k) pc(d[--k]);
}
static void pdec(long v)
{
    char d[24]; int k = 0;
    if (v < 0) { pc('-'); v = -v; }
    if (!v) d[k++] = '0';
    while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) pc(d[--k]);
}
static void kv(const char *k, long v)
{ u_puts("[R57] "); u_puts(k); u_puts("="); pdec(v); pc('\n'); }
static void kh(const char *k, long v)
{ u_puts("[R57] "); u_puts(k); u_puts("="); phex(v); pc('\n'); }

/* 这个全局变量落在 .data，链接地址在 USER_BASE 之后；fork 逐页复制时
 * 它所在的 4KB 页被复制进子进程独立物理页 → 子改它不影响父。 */
static volatile long g_shared = 0x1111;

int main(void)
{
    long me = u_getpid();
    kh("pid_before", me);
    kh("shared_before", g_shared);

    long pid = u_fork();
    if (pid != 0) kv("fork_ret", pid);    /* 只父打印；子进程用独立 key，避免覆盖判据字典 */

    if (pid == 0) {
        /* ---- 子进程：改自己的全局变量，然后体面退出 ---- */
        kv("child_forkret", pid);         /* 子视角 fork()=0（独立 key，不覆盖父的 fork_ret） */
        kh("child_shared_w", 0x2222);
        g_shared = 0x2222;
        kh("child_shared_r", g_shared);
        long cp = u_getpid();
        kv("child_pid", cp);
        u_puts("[R57] child_exit\n");
        u_procexit(7);            /* 切回父；父 wait 返回本子 pid */
        for (;;) { }              /* 不会到达 */
    }

    /* ---- 父进程：pid>0 ---- */
    if (pid < 0) { kv("fail", 1); sys_exit(1); }
    int status = -1;
    long got = u_wait(&status);
    kv("wait_ret", got);
    kv("status", status);
    kh("shared_after", g_shared);
    kv("pid_after", u_getpid());

    int pass = 0, fail = 0;
    if (pid >= 2) pass++; else fail++;              /* 子 pid 从 2 起 */
    if (got == pid) pass++; else fail++;            /* wait 返回子 pid */
    if (status == 7) pass++; else fail++;           /* 子退出码 */
    if (g_shared == 0x1111) pass++; else fail++;    /* 地址隔离：父内存未被子改 */
    if (u_getpid() == 1) pass++; else fail++;       /* 父仍是初始进程 */
    kv("pass", pass);
    kv("fail", fail);
    u_puts("[R57] done\n");
    sys_exit(fail ? 1 : 0);
}
