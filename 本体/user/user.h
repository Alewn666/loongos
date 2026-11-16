/* LoongOS 用户态程序头（阶段5② ELF 加载器 / 阶段8 POSIX 文件接口）
 * 用户程序 = 普通 C 文件，-nostdlib 编译，链在 USER_BASE 起。
 * 与内核的唯一通道是 SVC 系统调用（x8=号，x0/x1/x2=参数，返回值在 x0）：
 *   0 write(buf,len)              兼容老用法：x0=用户指针
 *   0 write(fd,buf,len)           POSIX 风格：x0=1/2 → 串口
 *   1 exit(code)
 *   2 open(path, flags)           → fd（读整份文件到内核缓冲，上限 512 字节）
 *   3 read(fd, buf, len)          → 实际读到的字节数，0=EOF
 *   4 close(fd)
 *   5 lseek(fd, off, whence)      → 新偏移（0=SET 1=CUR 2=END）
 * 指针越界/摸内核内存都会被内核拒绝或杀进程——见 user/bad.c 演示。 */
#ifndef LOONGOS_USER_H
#define LOONGOS_USER_H

typedef unsigned long u64;
typedef unsigned char u8;

/* 系统调用：x8=号，x0/x1=参数，返回值在 x0 */
static inline long sys_call(long nr, long a, long b)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

static inline long sys_call3(long nr, long a, long b, long c)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static inline long u_strlen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static inline void  u_puts(const char *s)  { sys_call(0, (long)s, u_strlen(s)); }
static inline void  sys_exit(long code)
{
    sys_call(1, code, 0);
    for (;;)
        __asm__ volatile("wfe");   /* 正常到不了这（exit 不返回） */
}

/* 参数区（内核装载时写好）：[0]=argc，[1..8]=argv[]，后面是字符串池。
   地址必须与 kernel.c 的 UARGS_ADDR 一致：USER_END(0x40400000) - 栈保留(0x1000) - 512 */
#define UARGV_ADDR 0x403fee00UL
static inline long u_argc(void) { return *(volatile long *)UARGV_ADDR; }
static inline const char *u_argv(long i)
{
    volatile long *p = (volatile long *)(UARGV_ADDR + 8);
    return (const char *)p[i];
}

/* POSIX 风格的薄封装（0~5 号，逐字保留） */
static inline long u_open(const char *path)        { return sys_call(2, (long)path, 0); }
static inline long u_read(long fd, void *b, long n) { return sys_call3(3, fd, (long)b, n); }
static inline long u_close(long fd)                { return sys_call(4, fd, 0); }
static inline long u_seek(long fd, long o, long w) { return sys_call3(5, fd, o, w); }
static inline long u_write(long fd, const void *b, long n) { return sys_call3(0, fd, (long)b, n); }

/* ---------------- 第五十五轮：扩容 syscall 6~18 ----------------
 * ABI 编号与 struct ustat 由 ../usys.h 统一定义（用户态/内核/宿主单测共用）。
 * 注意：这些封装只发 SVC，不做任何 libc 调用；内核侧按 x8 分发。 */
#include "../usys.h"

static inline long u_getcwd(char *b, long n)            { return sys_call(SYS_getcwd, (long)b, n); }
static inline long u_chdir(const char *p)              { return sys_call(SYS_chdir, (long)p, 0); }
static inline long u_getpid(void)                      { return sys_call(SYS_getpid, 0, 0); }
static inline long u_stat(const char *p, void *st)     { return sys_call(SYS_stat, (long)p, (long)st); }
static inline long u_fstat(long fd, void *st)          { return sys_call(SYS_fstat, fd, (long)st); }
static inline long u_dup(long fd)                      { return sys_call(SYS_dup, fd, 0); }
static inline long u_dup2(long oldfd, long newfd)      { return sys_call(SYS_dup2, oldfd, newfd); }
static inline long u_pipe(int *fds)                    { return sys_call(SYS_pipe, (long)fds, 0); }
static inline long u_mkdir(const char *p)              { return sys_call(SYS_mkdir, (long)p, 0); }
static inline long u_unlink(const char *p)             { return sys_call(SYS_unlink, (long)p, 0); }
static inline long u_rename(const char *o, const char *n) { return sys_call(SYS_rename, (long)o, (long)n); }
static inline long u_kill(long pid, long sig)          { return sys_call(SYS_kill, pid, sig); }
static inline long u_getdents(const char *p, char *b, long n) {
    return sys_call3(SYS_getdents, (long)p, (long)b, n);
}
static inline long u_creat(const char *p)              { return sys_call(SYS_creat, (long)p, 0); }

#endif
