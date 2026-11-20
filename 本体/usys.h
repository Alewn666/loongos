/* LoongOS 第五十五轮 —— syscall 编号与用户/内核共享结构（演进路线 §六 第 55 轮）
 * ---------------------------------------------------------------------------
 * 这份头是「用户态 ELF 与内核 svc_dispatch 之间」唯一的 ABI 契约。用户程序
 * （user 目录下的 C 文件，-nostdlib 编译）与内核 kernel.c 都 include 它，
 * 保证编号和 struct 布局逐字节一致；改这里等于改 ABI，必须两侧同步重新编译。
 *
 * 编号 0~5 是阶段5②/阶段8 的历史 syscall，行为逐字不变（老 ELF 程序
 * hello/cat/args/bad/hang 与 test_stage5b_elf.py 33 项判据全依赖它）。
 * 6~19 是第五十五轮新增；**刻意不复用 0~5 的任何分支**。
 *
 * 类型：C11 允许同型 typedef 重复（项目 fs.h/loongos.h/mm.h 已这么用），但为让
 * 「用户态 -nostdlib / 内核 / 宿主单测」三种编译单元都能独立 include 本头，
 * 这里自带一份最小 typedef，用 LOONGOS_USYS_TYPES 守卫与其它头互不干扰。
 */
#ifndef LOONGOS_USYS_H
#define LOONGOS_USYS_H

#ifndef LOONGOS_USYS_TYPES
#define LOONGOS_USYS_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

/* ---- syscall 号（x8）---- */
#define SYS_write      0    /* 历史：write(ptr,len) 老接口 / write(fd,buf,len) POSIX */
#define SYS_exit       1
#define SYS_open       2
#define SYS_read       3
#define SYS_close      4
#define SYS_lseek      5

#define SYS_getcwd     6    /* getcwd(buf,len) -> 长度，或 -1 */
#define SYS_chdir      7    /* chdir(path) -> 0 / -1 */
#define SYS_getpid     8    /* getpid() -> pid（单进程模型恒为 1） */
#define SYS_stat       9    /* stat(path, struct ustat *) -> 0 / -1 */
#define SYS_fstat     10    /* fstat(fd, struct ustat *) -> 0 / -1 */
#define SYS_dup       11    /* dup(fd) -> 新 fd / -1 */
#define SYS_dup2      12    /* dup2(old,new) -> new / -1 */
#define SYS_pipe      13    /* pipe(int fds[2]) -> 0 / -1；fds[0]读 fds[1]写 */
#define SYS_mkdir     14    /* mkdir(path) -> 0 / -1 */
#define SYS_unlink    15    /* unlink(path) -> 0 / -1 */
#define SYS_rename    16    /* rename(old,new) -> 0 / -1（不支持目录，见 fs_rename） */
#define SYS_kill      17    /* kill(pid,sig) -> 0 / -1（单进程模型，见实现注） */
#define SYS_getdents  18    /* getdents(path,buf,len) -> 写入字节数 / -1 */
#define SYS_creat     19    /* creat(path) -> 0 / -1（建空文件；供用户态写文件/rename 验证） */
#define SYS_vfsread   20    /* vfsread(path,buf,len) -> 字节数 / -1（读 /proc /dev 伪文件） */
#define SYS_vfsinfo   21    /* vfsinfo() -> 挂载点数量 */
/* 22~25 定义在 proc.h（第五十七轮进程模型），26/27 定义在 sched.h（58.0-B 调度器） */

/* ---- 第五十九轮 59.0-A：开发者 API 扩展层（syscall 28~35）----
 * 全部是「转发内核已有能力」的薄 syscall，不含新内核算法：
 *   - 不改 0~27 任何分支、不动 golden_behavior.txt；
 *   - 新号只追加在 svc_dispatch 末尾；用户态封装见 user/loslib.h。
 * 编号与结构同样是用户态/内核共享 ABI，改这里须两侧重新编译。 */
#define SYS_clock_gettime 28   /* clock_gettime(clk, struct utime*) -> 0 / -1；clk:0=墙钟 1=单调 */
#define SYS_getrandom     29   /* getrandom(buf,len) -> 实得字节数 / -1（virtio-rng，无设备则 -1） */
#define SYS_uname         30   /* uname(struct utsname*) -> 0 / -1 */
#define SYS_getppid       31   /* getppid() -> 父 pid（pid=1 的父返回 0） */
#define SYS_nanosleep     32   /* nanosleep(const struct utime*) -> 0 / -1（忙等，教学实现） */
#define SYS_fsstat        33   /* fsstat(struct ufsstat*) -> 0 / -1（内存盘块位图统计） */
#define SYS_meminfo       34   /* meminfo(struct umeminfo*) -> 0 / -1（kmalloc 堆统计） */
#define SYS_uptime        35   /* uptime(struct utime*) -> 0 / -1（单调递增时间） */

#define SYS_max       36    /* 号上限（不含） */

/* 时间结构（16B，秒+纳秒，定长无指针） */
struct utime {
    u64 sec;                    /* clk=0 墙钟 Unix 秒 / clk=1 单调秒 */
    u64 nsec;                   /* 纳秒部分 */
};
/* 系统信息（定长字符串，无指针，便于用户态按偏移解析） */
struct utsname {
    char sysname[16];           /* "LoongOS" */
    char release[16];           /* 版本串，如 "59.0-A" */
    char version[16];           /* 内核构建标识 */
    char machine[16];           /* "aarch64" */
};
/* 文件系统统计（内存盘） */
struct ufsstat {
    u32 used_bytes;
    u32 total_bytes;
    u32 blocks;
    u32 reserved;
};
/* 堆内存统计 */
struct umeminfo {
    u32 used_bytes;
    u32 peak_bytes;
    u32 live_objs;
    u32 fail_cnt;
};

/* ---- stat 的用户可见结构（32B，纯定长，无指针，方便用户态解析）----
 * 偏移：size@0 ino@4 type@8 mode@10 uid@12 nlink@14 mtime@16 _pad@20 wall@24。
 * wall 是 u64 需 8 对齐，故 mtime 后**显式**补 4B，避免不同编译器 padding 差异。
 * 用户态 u55_systest 按字节偏移只读前 20B，_pad/wall 不影响它。 */
struct ustat {
    u32 size;       /* 字节数 */
    u32 ino;        /* inode 号 */
    u16 type;       /* 1=目录 2=普通文件 0=未知（与 FS_T_* 一致） */
    u16 mode;       /* FM_* 位 */
    u16 uid;        /* 属主 uid */
    u16 nlink;      /* 硬链接/引用数（本轮简化恒 1） */
    u32 mtime;      /* 开机秒数（保 golden_behavior.txt 语义） */
    u32 _pad;       /* 显式补齐：让 wall 8 对齐，布局确定 */
    u64 wall;       /* 第五十四轮真实墙钟 Unix 秒，0=无 */
};

/* ---- pipe 容量（用户态 sh 拼管道时用它估缓冲）---- */
#define UPIPE_CAP     1024u

/* ---- 信号号（本轮只认 SIGTERM，其余拒）---- */
#define USIG_TERM     15

#endif /* LOONGOS_USYS_H */
