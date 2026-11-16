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
#define SYS_max       22    /* 号上限（不含） */

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
