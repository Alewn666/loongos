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

/* POSIX 风格的薄封装 */
static inline long u_open(const char *path)        { return sys_call(2, (long)path, 0); }
static inline long u_read(long fd, void *b, long n) { return sys_call3(3, fd, (long)b, n); }
static inline long u_close(long fd)                { return sys_call(4, fd, 0); }
static inline long u_seek(long fd, long o, long w) { return sys_call3(5, fd, o, w); }
static inline long u_write(long fd, const void *b, long n) { return sys_call3(0, fd, (long)b, n); }

#endif
