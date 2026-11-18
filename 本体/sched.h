/* LoongOS 第五十八轮 B 子里程碑（58.0-B）—— 单核抢占式调度地基：纯调度逻辑
 * ---------------------------------------------------------------------------
 * 演进路线把「抢占式调度」列为 2.3 shell 迁出与多用户程序并发的前置。真·抢占
 * （在 IRQ 里切用户进程帧）依赖独立内核栈、fs scratch 加锁等前置，按用户排期
 * 后置；本轮先落地可独立验收的调度地基：五态状态机 + 时间片 + 轮转 runqueue +
 * 可观测统计。与 proc.c（进程槽位/地址空间）平行，sched.c 只做「谁该跑」的决策，
 * 不碰 MMIO / 页表 / 异常帧，因此宿主可用 -std=gnu11 -Werror 独立单测。
 *
 * 设计取舍（沿用 54~58 轮一致的「地基先行 + 可复现判据 + 零扰动」）：
 *   ① 纯逻辑：struct scheduler 固定静态数组，无 malloc / 无 libc / 无硬件；
 *   ② 五态机对齐路线 58.0-B 的 RUNNABLE/RUNNING/SLEEPING/ZOMBIE 术语；
 *   ③ 轮转公平：sched_pick_next 从 next_slot 起环形扫描，N 个就绪实体轮流被选；
 *   ④ 时间片以 tick 为单位（tick=10Hz → SCHED_SLICE=10 即 1 秒）；到期只记统计
 *      与重置 slice，真正的硬抢占切换是后续（诚实边界，见报告）。
 */
#ifndef LOONGOS_SCHED_H
#define LOONGOS_SCHED_H

#ifndef LOONGOS_SCHED_TYPES
#define LOONGOS_SCHED_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

/* 进程调度状态（五态；S_FREE 表示槽位空闲，对齐 proc 的 P_FREE 语义） */
#define S_FREE      0u
#define S_RUNNABLE  1u
#define S_RUNNING   2u
#define S_SLEEPING  3u
#define S_ZOMBIE    4u

#define SCHED_NPROC   4u    /* 调度实体上限（与 proc.h 的 NPROC 对齐） */
#define SCHED_SLICE   10u   /* 默认时间片（tick 数；10Hz → 1 秒） */

/* ---- 58.0-B 新增 syscall 号（紧接 58.0-A 的 22~25；proc.h/usys.h 的 SYS_max→28）---- */
#define SYS_yield      26   /* yield() -> 让出 CPU 到下一个就绪进程，返回新 pid / 0 */
#define SYS_schedstat  27   /* schedstat(struct uschedstat *) -> 0 / -1 */

/* 用户可见的调度快照（纯定长、无指针，布局确定；内核 schedstat 逐字段写入）。
 * 偏移：每个 ent 20B × 4 = 80B，随后 current/ctx/expires/yields 各 4B = 96B。 */
struct usched_ent {
    u32 pid;
    u32 state;
    u32 slice;
    u32 ticks_run;
    u32 switches;
};
struct uschedstat {
    struct usched_ent ent[SCHED_NPROC];   /* 80B */
    u32 current;                          /* 80 */
    u32 ctx_switches;                     /* 84 */
    u32 slice_expires;                    /* 88 */
    u32 yields;                           /* 92 */
};

/* 一个调度实体：pid + 状态 + 时间片 + 统计（与 proc 槽位按 pid 对齐，平行视图） */
struct sched_ent {
    u32 pid;
    u32 state;
    u32 slice;          /* 剩余时间片（tick） */
    u32 ticks_run;      /* 累计运行 tick */
    u32 switches;       /* 被切上 CPU 的次数 */
};

/* 调度器：固定实体表 + 轮转指针 + 全局统计 */
struct scheduler {
    struct sched_ent ent[SCHED_NPROC];
    u32 current;        /* 当前 RUNNING 的 pid；0 = 无 */
    u32 next_slot;      /* 轮转 pick 的起始槽（公平性核心） */
    u32 ctx_switches;   /* 累计上下文切换次数 */
    u32 slice_expires;  /* 累计时间片到期次数 */
    u32 yields;         /* 累计主动让出次数 */
};

/* ---- 纯逻辑 API（sched.c 实现，宿主可测）---- */
void sched_init(struct scheduler *s);
/* 新建实体 → RUNNABLE：返回槽号(0..N-1)，表满/重复 pid 返回 -1 */
int  sched_add(struct scheduler *s, u32 pid);
/* 移除实体（置 S_FREE）：成功 0，找不到 -1；若移的是 current 则 current 归 0 */
int  sched_remove(struct scheduler *s, u32 pid);
/* 状态迁移（非法迁移返回 -1）：S_FREE 只能经 sched_add/remove；S_ZOMBIE 只能 remove */
int  sched_set_state(struct scheduler *s, u32 pid, u32 st);
/* 按 pid 找实体，找不到返回 0 */
struct sched_ent *sched_find(struct scheduler *s, u32 pid);
/* 轮转挑选下一个 RUNNABLE 实体（从 next_slot 环形扫描）；不改其状态，返回 0 表示无 */
struct sched_ent *sched_pick_next(struct scheduler *s);
/* 推进一个 tick：当前 RUNNING 的 slice--、ticks_run++；到期则重置 slice 且计数，
 * 返回 1=时间片到期 / 0=未到期 / -1=无 RUNNING 实体 */
int  sched_tick(struct scheduler *s);
/* 主动让出：当前 RUNNING→RUNNABLE、统计 yields++，pick_next→RUNNING；
 * 返回切上的新 pid（0=无其他就绪实体，当前保持 RUNNING） */
u32  sched_yield(struct scheduler *s);

#endif /* LOONGOS_SCHED_H */
