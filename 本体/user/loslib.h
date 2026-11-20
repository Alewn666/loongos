/* LoongOS 开发者库（loslib.h）—— 第三方应用开发的统一入口
 * ---------------------------------------------------------------------------
 * 用法：用户程序只 include 本头（它已包含 user.h/usys.h/proc.h/sched.h），
 *       用 los_* 系列函数即可，不必关心 SVC 细节。
 *
 *   #include "loslib.h"
 *   int main(void) {
 *       struct utsname u; los_uname(&u);
 *       los_puts(u.sysname);
 *       struct utime t; los_clock_gettime(0, &t);
 *       los_printf_time(&t);
 *       los_exit(0);
 *   }
 *
 * 编译：-nostdlib 静态 ELF，链在 USER_BASE（见 build.sh 的 UCFLAGS）。
 * 约定：所有函数返回 0=成功 / -1=失败（getrandom/uptime 除外见注释）。
 * 本库只发 SVC，不做任何 libc 调用，且不触碰内核内存（内核侧逐一校验）。
 */
#ifndef LOONGOS_LOSLIB_H
#define LOONGOS_LOSLIB_H

#include "user.h"        /* sys_call/sys_call3 + u_* 基础封装 + usys.h/proc.h/sched.h */

/* ---- 字符串/输出小工具（无 libc）---- */
static inline long los_strlen(const char *s) { return u_strlen(s); }
static inline void los_puts(const char *s)   { u_puts(s); }
static inline void los_putc(char c)          { char b[2] = { c, 0 }; u_puts(b); }
static inline void los_puthex(u64 v)
{
    char b[17]; int n = 0;
    if (!v) b[n++] = '0';
    while (v) { int d = (int)(v & 15); b[n++] = d < 10 ? (char)('0'+d) : (char)('a'+d-10); v >>= 4; }
    b[n] = 0;
    /* 反转 */
    for (int i = 0, j = n - 1; i < j; i++, j--) { char t=b[i]; b[i]=b[j]; b[j]=t; }
    u_puts(b);
}
static inline void los_putdec(u64 v)
{
    char b[21]; int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    b[n] = 0;
    for (int i = 0, j = n - 1; i < j; i++, j--) { char t=b[i]; b[i]=b[j]; b[j]=t; }
    u_puts(b);
}

/* ---- 基础 I/O（沿用 user.h 的 u_*）---- */
static inline long los_write(const char *s)          { return u_puts(s), los_strlen(s); }
static inline long los_open(const char *p)           { return u_open(p); }
static inline long los_read(long fd, void *b, long n){ return u_read(fd, b, n); }
static inline long los_close(long fd)                { return u_close(fd); }
static inline void los_exit(long code)               { sys_exit(code); }

/* ---- 文件/目录（6~21 号）---- */
static inline long los_getcwd(char *b, long n)        { return u_getcwd(b, n); }
static inline long los_chdir(const char *p)           { return u_chdir(p); }
static inline long los_mkdir(const char *p)           { return u_mkdir(p); }
static inline long los_unlink(const char *p)          { return u_unlink(p); }
static inline long los_rename(const char *o, const char *n) { return u_rename(o, n); }
static inline long los_creat(const char *p)           { return u_creat(p); }
static inline long los_getdents(const char *p, char *b, long n) { return u_getdents(p, b, n); }
static inline long los_vfsread(const char *p, void *b, long n)  { return u_vfsread(p, b, n); }

/* ---- 进程/调度（22~27 号）---- */
static inline long los_fork(void)                     { return u_fork(); }
static inline long los_wait(int *st)                  { return u_wait(st); }
static inline void los_procexit(long code)            { u_procexit(code); }
static inline long los_execve(const char *p, const char *argline) { return u_execve(p, argline); }
static inline long los_yield(void)                    { return u_yield(); }
static inline long los_getpid(void)                   { return u_getpid(); }

/* ---- 59.0-A 开发者 API（28~35 号，纯转发已有内核能力）---- */
/* 时钟：clk 0=墙钟(Unix秒) 1=单调秒；成功 0 / 失败 -1 */
static inline long los_clock_gettime(int clk, struct utime *t) {
    return sys_call(SYS_clock_gettime, (long)t, clk);
}
/* 真随机数：成功=实得字节数(可能<len)，无 virtio-rng 设备= -1 */
static inline long los_getrandom(void *buf, unsigned long len) {
    return sys_call(SYS_getrandom, (long)buf, (long)len);
}
/* 系统信息：成功 0 / 失败 -1 */
static inline long los_uname(struct utsname *u) { return sys_call(SYS_uname, (long)u, 0); }
/* 父 pid：根进程返回 0 */
static inline long los_getppid(void) { return sys_call(SYS_getppid, 0, 0); }
/* 休眠（教学忙等，上限 2 秒）：成功 0 / 失败 -1 */
static inline long los_nanosleep(const struct utime *t) { return sys_call(SYS_nanosleep, (long)t, 0); }
/* 文件系统统计：成功 0 / 失败 -1 */
static inline long los_fsstat(struct ufsstat *s) { return sys_call(SYS_fsstat, (long)s, 0); }
/* 堆内存统计：成功 0 / 失败 -1 */
static inline long los_meminfo(struct umeminfo *m) { return sys_call(SYS_meminfo, (long)m, 0); }
/* 单调运行时间：成功 0 / 失败 -1 */
static inline long los_uptime(struct utime *t) { return sys_call(SYS_uptime, (long)t, 0); }

/* ---- 便捷打印：把 utime 打印成 "秒.n纳秒" ---- */
static inline void los_print_time(const struct utime *t)
{
    los_putdec(t->sec); los_putc('.'); los_putdec(t->nsec);
}

#endif /* LOONGOS_LOSLIB_H */
