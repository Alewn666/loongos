/* LoongOS 第五十五轮 —— 统一 fd / pipe 对象层（演进路线 §六 第 55 轮）
 * ---------------------------------------------------------------------------
 * 为什么独立成文件：
 *   ① fd 表、pipe 环形缓冲、dup/dup2/close 的引用计数是**纯逻辑**，抽出来
 *      就能用 cc 在宿主上 -Werror 单测（文档 §八「不接受串口打印说成功」）；
 *   ② pipe 的环形缓冲用第五十四轮 kmalloc/kfree 动态分配 —— 这是「动态内存
 *      地基」第一次被真实消费，且 close 后零泄漏可被 mmstat 量化验收；
 *   ③ 内核 svc_dispatch 只做「用户指针校验 + 分发」，不掺和对象管理。
 *
 * 与历史 g_fd 表（0~5 号 syscall 用，NFDS=6，fd 3..5）的关系：
 *   - 历史表**完全不动**，老 ELF 程序 / test_stage5b_elf.py 33 项判据零风险；
 *   - 本层 fd 从 UFD_NEWBASE(=6) 起分配，与历史表编号空间不重叠；
 *   - 0/1/2 是 console；dup2(old, 1) 会把 g_ofd[1] 从 CONSOLE 换成 PIPE_W，
 *     svc_dispatch 的 write/read 分支查到非 CONSOLE 才走本层 —— 未 dup2 时
 *     g_ofd[0..2] 恒 CONSOLE，钩子不触发，0~5 号行为逐字不变。
 *
 * 单线程模型（进程模型是第 57 轮）：pipe 不做阻塞/调度。write 写入
 * min(n, 剩余空间) 字节并返回实际写入数；read 读 min(n, 已有字节) 并返回
 * 实际读取数，空 pipe 返回 0。用户态 sh 按「写满 → 关写端 → 读」分阶段
 * 装配 ls|grep|wc，正好不需要阻塞语义。
 */
#ifndef LOONGOS_USYSFD_H
#define LOONGOS_USYSFD_H

#ifndef LOONGOS_USYSFD_TYPES
#define LOONGOS_USYSFD_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

#define UFD_MAX      24     /* fd 0..23：0/1/2 console，3..5 留给历史 g_fd，6..23 给新对象 */
#define UFD_NEWBASE  6      /* 新对象 fd 从这里起（避开历史 g_fd 的 3..5） */
#define UPIPE_MAX    8      /* 同时存在的 pipe 对象上限（8 pipe x 2 fd = 16 槽，6..21 够放） */
#define UPIPE_RING   1024u  /* 每个 pipe 环形缓冲字节数（kmalloc） */

enum {
    UFD_FREE   = 0,
    UFD_CONSOLE = 1,        /* fd 0/1/2 默认：走串口 */
    UFD_PIPE_R = 2,
    UFD_PIPE_W = 3
};

/* 每次 run 用户程序前调用：fd0/1/2=console，其余 free，所有 pipe 对象复位。
   注意：不主动 kfree 上一次残留的 ring —— ufd_reset 会遍历释放，保证跨 run
   零泄漏（这正是 mmstat 能验的）。 */
void ufd_init(void);
void ufd_reset(void);

/* console 输出回调注入：内核传 uart 输出函数，宿主单测传内存缓冲函数。
   未注入时 console 写静默丢弃（返回 -1）。 */
typedef void (*ufd_console_fn)(const u8 *buf, u32 n);
void ufd_set_console(ufd_console_fn fn);

/* pipe(fds)：fds[0]=读端 fd，fds[1]=写端 fd；返回 0 / -1。 */
int  ufd_pipe(int fds[2]);
int  ufd_dup(int oldfd);                 /* 返回最小空闲新 fd / -1 */
int  ufd_dup2(int oldfd, int newfd);     /* 返回 newfd / -1 */
int  ufd_close(int fd);                  /* 0 / -1 */

long ufd_read (int fd, u8 *dst, u32 n); /* 实际读到的字节数 / -1 */
long ufd_write(int fd, const u8 *src, u32 n); /* 实际写入字节数 / -1 */

/* svc_dispatch 的钩子查询：返回 fd 的对象类型（UFD_*）。
   fd 0/1/2 未经 dup2 时恒 UFD_CONSOLE —— 钩子据此决定是否走本层。 */
int  ufd_kind(int fd);

/* pipe 资源水位（供判据脚本 / 自检量化「零泄漏」） */
int  ufd_live_pipes(void);              /* 仍被引用的 pipe 对象数 */
u32  ufd_pipe_bytes(int fd);            /* 该 fd 所属 pipe 当前缓冲字节数 */
u32  ufd_alloc_fail(void);              /* kmalloc 失败累计 */

#endif /* LOONGOS_USYSFD_H */
