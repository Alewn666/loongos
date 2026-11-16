/* LoongOS 第五十七轮 —— 进程模型地基（演进路线 §3.2 / 六 第 57 轮）
 * ---------------------------------------------------------------------------
 * 路线文档把「进程模型」列为最硬的一块：每进程 TTBR0 + fork/execve/wait +
 * 每进程 fd 表 + 僵尸回收。本头是 kernel.c 与 proc.c 之间的 ABI 契约，
 * 同时被宿主单测（test_proc_host.c，-std=gnu11 -Werror）独立 include。
 *
 * 设计取舍（全部沿用第 54~56 轮的「地基先行 + 可复现判据 + 零扰动」习惯）：
 *   ① proc.c 只做**纯逻辑**（进程槽位状态机、僵尸回收、用户空间全拷贝）。
 *      不碰 MMIO / TTBR0 / kmalloc —— 这些硬件 glue 留在 kernel.c。
 *      于是「进程表正确」「fork 全拷贝后父子互不串写」能在 PC 上用 -Werror
 *      单测复现（文档第八节第 3 条：不接受串口自报成功）。
 *   ② 单内核栈 + 合作式切换：父子共享一条内核栈，异常帧内容整体拷贝实现
 *      上下文切换（boot.S 无需改动）。并发/抢占式多进程列为诚实边界。
 *   ③ 同时最多一个活跃子进程（合作串行：父 fork → 父 wait 让出 → 子跑 →
 *      子 procexit 切回父并留僵尸 → wait 回收）。兄弟并发/嵌套 fork 留后续。
 *
 * 帧内容用 opaque 字节数组（800B），不依赖 kernel.c 的 struct regs 定义，
 * 让 proc.c 在宿主侧也能独立编译。kernel.c 用 fs_memcpy 填/取。
 */
#ifndef LOONGOS_PROC_H
#define LOONGOS_PROC_H

#ifndef LOONGOS_PROC_TYPES
#define LOONGOS_PROC_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

/* 进程状态（简化三态：无独立 SLEEP，合作式等待由父让出实现） */
#define P_FREE    0
#define P_RUN     1
#define P_ZOMBIE  2

/* 进程数：pid 1 = 初始进程（shell/首次用户程序），2..NPROC 为 fork 出的子进程 */
#define NPROC     4
#define PROC_FRAME_BYTES 800u
#define PROC_NPAGES      20u     /* 每子进程独立用户物理页上限（代码16+栈2+余量） */

/* 一个进程槽位。user_pt 是 kernel.c 私有 per-proc 页表的不透明句柄
 * （宿主单测里可为 0，纯逻辑不解读它）。 */
struct proc {
    u32 pid;
    u32 state;
    u32 ppid;
    int exit_code;
    u64 user_pt;                 /* opaque：子进程用户 L3 表物理地址 */
    u8  frame[PROC_FRAME_BYTES]; /* 被换出时保存的异常帧（整帧拷贝） */

    /* ---- 以下运行期字段由 kernel.c 管理；proc.c 纯逻辑不解读（宿主单测保持纯粹）---- */
    u64 user_sp;                       /* 用户栈指针 sp_el0（合作式切换时换入换出） */
    u64 l3_raw;                        /* 子进程 L3 表 kmalloc 原始指针（kfree 用） */
    u64 pages[PROC_NPAGES];            /* 子进程独立用户物理页（页对齐） */
    u64 page_raw[PROC_NPAGES];         /* 对应 kmalloc 原始指针 */
    int  npages;
};

/* ---- 纯逻辑 API（proc.c 实现，宿主可测）---- */
/* 清空整张表：所有槽 P_FREE，pid=0。初始化静默，不打印。 */
void proc_init(struct proc *tab, int n);
/* 分配一个 pid（从 2 起找空槽），成功返回 pid，表满返回 -1。 */
int  proc_alloc(struct proc *tab, int n, u32 ppid);
/* 按 pid 找槽位，找不到返回 0。 */
struct proc *proc_find(struct proc *tab, int n, u32 pid);
/* 释放一个槽位（置 P_FREE / pid=0）。返回 0 成功，-1 非法。 */
int  proc_free(struct proc *p);
/* 把 pid 标记成僵尸并记录退出码。返回 0 成功、-1 找不到。 */
int  proc_zombie(struct proc *tab, int n, u32 pid, int code);
/* 统计 ppid 名下僵尸子进程数量。 */
int  proc_zombie_count(const struct proc *tab, int n, u32 ppid);
/* 回收 ppid  name 名下任意一个僵尸子进程：写回退出码与子进程 pid，
 * 释放槽位，返回 0；无僵尸返回 -1。 */
int  proc_reap(struct proc *tab, int n, u32 ppid, int *code, u32 *child_pid);

/* ---- 用户空间全拷贝（fork 的核心；通过回调注入读写，宿主可测）----
 * rd/wr 逐块搬移 [base, base+bytes)。调用方用 rd 读父进程用户内存、
 * wr 写子进程用户内存；任一块失败立即返回 -1。成功返回 0。
 * kernel.c 传入的 rd/wr 会走各自进程的页表（父 VA=PA、子经 l3_user 映射），
 * 于是「子改自己变量，父不变」在真实硬件上成立；宿主单测用两个数组当 ctx。 */
typedef int (*proc_mem_io)(u64 va, void *buf, u32 n, void *ctx);
int proc_copy_userspace(u64 base, u32 bytes, u32 chunk,
                        proc_mem_io rd, void *rctx,
                        proc_mem_io wr, void *wctx);

/* ---- 新增 syscall 号（紧接第 56 轮的 20/21；0~21 语义不变）---- */
#define SYS_fork      22   /* fork() -> 子返回 0，父返回子 pid，失败 -1 */
#define SYS_execve    23   /* execve(path, argv_packed) -> 成功不返回(覆盖当前进程)，失败 -1 */
#define SYS_wait      24   /* wait(int *status) -> 回收到子 pid / -1（无子进程或全RUN） */
#define SYS_procexit  25   /* procexit(code) -> 当前子进程僵尸化并切回父（父从 wait 返回） */
#define SYS_max       26

/* wait 写回的 status：高 8 位退出码，低 8 位标志 */
#define WST_CODE(st)  (((u32)(st) >> 8) & 0xFF)

#endif /* LOONGOS_PROC_H */
