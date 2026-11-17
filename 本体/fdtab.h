/* LoongOS 第五十八轮 58.0-A —— 每进程文件描述符表 + 打开文件描述(OFD)引用语义
 * ---------------------------------------------------------------------------
 * 演进路线 §3.2 步骤④「全局 g_fd 改 proc->files」+ 用户 58.0-A 验收项①。
 *
 * 设计契约（Unix 语义，尤其 fork 后父子**共享打开文件描述**而非复制整数）：
 *   - struct ofd  = 打开文件描述：{ ino, off, len, ref, used }。off 是文件读写
 *     偏移，被所有引用它的 fd 共享（父子 fork 后子 read 推进 off，父 read 接着读）。
 *   - struct fd_table = 每进程一张的描述符表：fd 槽 -> ofd 下标(+1，0=空)。
 *     fd 号是**每进程独立**的：父子各有自己的 0/1/2 和已打开 fd 槽。
 *   - fork：复制父 fd_table 的槽位数组到子表，并对每个被引用的 ofd 做 ref++。
 *     于是父子 fd 号相同、指向同一 ofd（共享 off），但各自的槽位可独立 close。
 *   - close(fd)：槽位对应 ofd 的 ref--，ref 归零才把 ofd 标 FREE（真正释放）；
 *     fd 槽清 0。close 一个进程自己的 fd 不影响另一进程（除非 ref 归零）。
 *   - exit/回收：把整张表的每个 ofd ref-- 后清空表。
 *
 * 与历史 g_fd 的关系（零扰动红线）：
 *   - pid=1 初始进程仍用 kernel.c 里的全局 g_fd（老 syscall 0/2/3/4/5 与
 *     test_stage5b 33 项逐字不变）。本模块只服务 fork 出的子进程 pid>=2。
 *   - 不碰 usysfd.c 的 pipe 层（fd 6..23），不碰 g_ofd/g_pipe，老管道演示零风险。
 *
 * freestanding 纯逻辑：不 include 任何内核头、不碰 MMIO/kmalloc/串口，
 * 宿主用 cc -std=gnu11 -Wall -Wextra -Werror 独立编译 test_fdtab_host.c 验证。
 */
#ifndef LOONGOS_FDTAB_H
#define LOONGOS_FDTAB_H

#ifndef LOONGOS_FDTAB_TYPES
#define LOONGOS_FDTAB_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

#define FDT_NFD    6u    /* 每进程 fd 0..5（与老 g_fd 的 NFDS=6 对齐） */
#define FDT_NOFD   12u   /* 全局打开文件描述池容量（父子 fork 各占一份引用） */
#define FDT_KIND_FREE  0
#define FDT_KIND_CONS  1     /* fd 0/1/2 串口 console（伪 ofd，不占文件 ino） */
#define FDT_KIND_FILE  2     /* 普通文件：ino/off/len 有效 */

/* 打开文件描述：所有引用它的 fd 共享 off。ref=引用它的 fd 总数（跨进程）。 */
struct ofd {
    u32 ino;             /* fs inode 号；console 时为 0 */
    u32 off;             /* 当前读写偏移（共享） */
    u32 len;             /* 文件大小 */
    u16 ref;             /* 引用计数 */
    u8  kind;            /* FDT_KIND_* */
    u8  used;            /* 1=该池项在用 */
};

/* 每进程描述符表：slot[fd] = ofd 下标 + 1（0=该 fd 空闲）。 */
struct fd_table {
    u8 slot[FDT_NFD];
};

/* ---- OFD 池 API（全局，内核持有一张 g_ofds）---- */
void fdt_pool_init(struct ofd *pool, u32 n);
/* 新建/复用一个文件 ofd：ino/off=0/len 给定，ref 置 1，返回池下标；池满 -1。 */
int  fdt_ofd_new_file(struct ofd *pool, u32 n, u32 ino, u32 len);
/* 取一个 console ofd（池里找空闲项建 FDT_KIND_CONS），返回下标；池满 -1。 */
int  fdt_ofd_new_console(struct ofd *pool, u32 n);
/* 增/减引用。unref 在 ref 归零时把 used 清 0，返回 1=已释放 0=仍被引用。 */
int  fdt_ofd_ref(struct ofd *o);
int  fdt_ofd_unref(struct ofd *o);

/* ---- 每进程 fd_table API ---- */
/* 初始进程：fd 0/1/2 指向三个独立 console ofd（经回调），其余空。返回 0。 */
int  fdt_init(struct fd_table *t, struct ofd *pool, u32 n);
/* 清空本表：对每个 slot 的 ofd unref，然后 slot 全 0。 */
void fdt_destroy(struct fd_table *t, struct ofd *pool, u32 n);
/* 分配最小空闲 fd，指向给定 ofd（ref++），返回 fd；无空闲 -1。 */
int  fdt_install(struct fd_table *t, struct ofd *pool, u32 n, u32 ofd_idx);
/* 关闭 fd：对应 ofd unref（归零则释放），slot 清 0。返回 0 / -1（fd 非法）。 */
int  fdt_close(struct fd_table *t, struct ofd *pool, u32 n, u32 fd);
/* 取 fd 对应的 ofd 下标（+1 语义已解开）；fd 非法/空闲返回 -1。 */
int  fdt_get(struct fd_table *t, u32 fd);
/* dup：找一个最小空闲 fd 指向 src 的同一 ofd（ref++）。返回新 fd / -1。 */
int  fdt_dup(struct fd_table *t, struct ofd *pool, u32 n, u32 src);
/* dup2：把 src 的 ofd 装到 dst（若 dst 原有关闭），返回 dst / -1。 */
int  fdt_dup2(struct fd_table *t, struct ofd *pool, u32 n, u32 src, u32 dst);
/* fork：把父表所有槽位复制到子表，每个被引用的 ofd ref++。返回复制的 fd 数。
   前提：子表必须是未初始化的全 0 表（调用方先 fs_memset 或静态零）。 */
int  fdt_fork(const struct fd_table *parent, struct fd_table *child,
              struct ofd *pool, u32 n);

/* ---- 统计（判据量化零泄漏）---- */
int  fdt_pool_live(const struct ofd *pool, u32 n);   /* used=1 的 ofd 数 */
u32  fdt_pool_refsum(const struct ofd *pool, u32 n); /* 所有 used ofd 的 ref 之和 */

#endif /* LOONGOS_FDTAB_H */
