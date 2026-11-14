#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS 阶段9 —— SMP（多核）正式版
 *
 * 设计目标（用户原话：「把核心都用起来，适配多线程多核心处理。」）：
 *   不让从核只空转计数，而是把可安全并行的真实工作分派下去。
 *
 * 已落地的关键结论（都是踩坑实测，不是推测）：
 *   1. PSCI conduit：默认 `-machine virt`（EL1、无 EL2/EL3）下，在 EL1 执行
 *      `smc #0` 是 UNDEFINED，会刷异常转储。QEMU TCG 与 KVM 都通过 `hvc #0`
 *      投递 PSCI，所以本版只用 hvc，绝不碰 smc。
 *   2. 从核睡眠 / 唤醒（wfi + SGI + 每核定时唤醒 三重策略）：
 *      - QEMU TCG 下 `wfe` 是 no-op（从核忙等、宿主 CPU ≈300%），所以不用 wfe/sev。
 *      - `wfi` 在 DAIF 屏蔽且无 pending 中断时能真 halt（宿主 CPU ≈0%），平时从核
 *        就 wfi 真睡。
 *      - 唤醒用 SGI/IPI（GICv2 SGIR / GICv3 ICC_SGI1R IRM=1 广播）优先；但实测
 *        GICv3 某些拓扑下 SGI 广播根本不到达从核（cpu 显示 IPI=0，任务卡死）。
 *      - 因此再加「每核私有物理定时器 TIMER_PPI(30)」做低频看门狗唤醒
 *        （SMP_TIMER_HZ=250），作为拓扑无关的可靠后援：任何 GIC 下最迟 4ms 内
 *        从核必被唤醒认领工作。关键：IRQ handler 必须重装 cntp_tval（清挂起），
 *        否则 wfi 立即返回 → 满速空转烧核（宿主 ≈275%）。
 *   3. 多核下 UART(SPI 33) 必须由 GIC 固定路由到 CPU0，否则 1-of-N 分发会把
 *      串口中断丢给从核，主核 shell 收不到输入（whoami/ls 全哑）。这个修复
 *      在 kernel.c 的 gic_init() 里做（GICv2 ITARGETSR / GICv3 IROUTER）。
 *
 * 工作模型：
 *   - 主核准备一个 job（缓冲区 + 分区数 + 类型），先广播 SGI 唤醒所有从核，
 *     自己同时算 partition 0（CPU0 也出力），再轮询 G_job.done 直到所有核完成。
 *   - 每个从核被 SGI 唤醒后，按自己的 MPIDR.aff0 认领对应分区，算完把部分结果
 *     写进 G_job.part[cpu]，原子递增 G_job.done，再回 wfi。
 *   - 工作负载刻意选「分区互不相交、只读输入、只写自己那格 part」——零共享写
 *     冲突，不需要锁住 fs/UART/framebuffer 等未加锁全局状态，安全可并行。
 *
 * ⚠️ 两条硬约束（沿用）：
 *   - cortex-a57 没有 LSE 原子指令，且我们 -nostdlib（没有 libgcc 的
 *     __aarch64_ldadd），原子读改写必须手写 ldxr/stxr。
 *   - MMU 未开时内存是 Device 属性，ldaxr/stxr 不可用 → 锁用 smp_locks_on 门住。
 */

typedef unsigned long u64;
typedef unsigned int  u32;
typedef unsigned char u8;

#define SMP_MAX 4

/* ---------------- 控制台锁 + 启用开关 ---------------- */
volatile int console_lock = 0;
volatile int smp_locks_on = 0;          /* 调用点都在 MMU 打开之后才置 1 */
volatile int smp_no_conduit = 0;        /* 1 = 本机无可用 PSCI conduit */

/* ---------------- per-CPU 私有状态 ---------------- */
struct percpu {
    u64 cpu_id;
    u64 tick;          /* 本核软件 tick（CPU0 由 shell 循环推、从核不跑定时器故为 0） */
    u64 irq_depth;
    u64 online;
    u64 stack_top;
};
struct percpu smp_pcpu[SMP_MAX];

u64 smp_stacks[SMP_MAX][512] __attribute__((aligned(16)));   /* 每核 4KB 栈 */

volatile u64 smp_online[SMP_MAX];      /* 各核是否上线（CPU0 恒 1） */
volatile u64 smp_counter[SMP_MAX];     /* 各核被唤醒（收到 SGI）累计次数 */
volatile u64 smp_jobs_done[SMP_MAX];   /* 各核完成任务累计次数 */
volatile u64 smp_ipi_hits[SMP_MAX];    /* 各核收到 SGI 的次数（含非任务唤醒） */
volatile u64 smp_work_cycles[SMP_MAX]; /* 各核最近一次任务耗时（cntpct 周期） */

static inline struct percpu *this_cpu(void)
{
    struct percpu *p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}

void smp_pcpu_setup(u64 cpu)
{
    if (cpu >= SMP_MAX) cpu = 0;
    smp_locks_on = 1;                    /* 调用点都在 MMU 打开之后 */
    smp_pcpu[cpu].cpu_id = cpu;
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&smp_pcpu[cpu]));
    __asm__ volatile("isb");
}

/* 原子递增 32 位（仅 MMU 打开后、smp_locks_on=1 时调用；用于 G_job.done 计数） */
static void atomic_inc(volatile int *p)
{
    int old, nv; int fail;
    do {
        __asm__ volatile("ldxr %w0, [%1]" : "=r"(old) : "r"(p) : "memory");
        nv = old + 1;
        __asm__ volatile("stxr %w0, %w2, [%1]" : "=&r"(fail) : "r"(p), "r"(nv) : "memory");
    } while (fail);
}

u64 smp_pcpu_tick(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX) return 0;
    return *(volatile u64 *)&smp_pcpu[cpu].tick;
}

/* 主核节拍计数（shell_loop / desktop_run 调用，仅 CPU0 写，无竞争） */
void smp_tick_inc(void)
{
    struct percpu *p = this_cpu();
    if (!p) return;
    p->tick++;
}

/* ---------------- 自旋锁（用于未来要保护的共享状态） ---------------- */
void spin_lock(volatile int *lk)
{
    int v, fail;
    for (;;) {
        __asm__ volatile("ldaxr %w0, [%1]" : "=r"(v) : "r"(lk) : "memory");
        if (v == 0) {
            __asm__ volatile("stxr %w0, %w2, [%1]" : "=&r"(fail) : "r"(lk), "r"(1) : "memory");
            if (fail == 0) return;
        }
        __asm__ volatile("yield");
    }
}
void spin_unlock(volatile int *lk)
{
    __asm__ volatile("stlr wzr, [%0]" :: "r"(lk) : "memory");
}

/* ---------------- 从核入口（汇编） ---------------- */
__asm__(
    ".text\n"
    ".global smp_secondary_entry\n"
    "smp_secondary_entry:\n"
    "  mrs x9, mpidr_el1\n"
    "  and x9, x9, #0xff\n"                /* aff0 = 核号 */
    "  adrp x10, smp_stacks\n"
    "  add  x10, x10, :lo12:smp_stacks\n"
    "  add  x10, x10, x9, lsl #12\n"       /* + cpu*4KB */
    "  add  x10, x10, #4096\n"             /* 栈顶 */
    "  mov  sp, x10\n"
    "  ldr x12, =smp_ttbr0\n"
    "  ldr x12, [x12]\n"
    "  msr ttbr0_el1, x12\n"
    "  ldr x12, =smp_tcr\n"
    "  ldr x12, [x12]\n"
    "  msr tcr_el1, x12\n"
    "  ldr x12, =smp_mair\n"
    "  ldr x12, [x12]\n"
    "  msr mair_el1, x12\n"
    "  adrp x12, smp_vectors\n"
    "  add  x12, x12, :lo12:smp_vectors\n"
    "  msr vbar_el1, x12\n"
    "  dsb sy\n"
    "  tlbi vmalle1\n"
    "  dsb sy\n"
    "  isb\n"
    "  mrs x12, sctlr_el1\n"
    "  orr x12, x12, #(1 << 0)\n"          /* M */
    "  orr x12, x12, #(1 << 2)\n"          /* C */
    "  orr x12, x12, #(1 << 12)\n"         /* I */
    "  msr sctlr_el1, x12\n"
    "  isb\n"
    "  mov x0, x9\n"
    "  b smp_secondary_main\n"
);

/* ---------------- 从核向量表（AArch64：16 个槽，每槽 0x80 字节） ----------------
 * 每槽 .balign 128，VBAR 2KB 对齐（真机 KVM 会取整）。
 * 当前 EL / SP0(SPx) 的 IRQ 槽（0x080 / 0x280）都进 smp_irq_entry，
 * 从核只在那里收 SGI 唤醒。 */
__asm__(
    ".text\n"
    ".balign 2048\n"
    ".global smp_vectors\n"
    "smp_vectors:\n"
    "  .balign 128\n  b smp_ign\n"          /* 0x000 当前EL SP0 Sync   */
    "  .balign 128\n  b smp_irq_entry\n"     /* 0x080 当前EL SP0 IRQ    */
    "  .balign 128\n  b smp_ign\n"          /* 0x100 当前EL SP0 FIQ    */
    "  .balign 128\n  b smp_ign\n"          /* 0x180 当前EL SP0 SError */
    "  .balign 128\n  b smp_ign\n"          /* 0x200 当前EL SPx Sync   */
    "  .balign 128\n  b smp_irq_entry\n"     /* 0x280 当前EL SPx IRQ    */
    "  .balign 128\n  b smp_ign\n"          /* 0x300 当前EL SPx FIQ    */
    "  .balign 128\n  b smp_ign\n"          /* 0x380 当前EL SPx SError */
    "  .balign 128\n  b smp_ign\n"          /* 0x400 低EL A64 Sync     */
    "  .balign 128\n  b smp_ign\n"          /* 0x480 低EL A64 IRQ      */
    "  .balign 128\n  b smp_ign\n"          /* 0x500 低EL A64 FIQ      */
    "  .balign 128\n  b smp_ign\n"          /* 0x580 低EL A64 SError   */
    "  .balign 128\n  b smp_ign\n"          /* 0x600 低EL A32 Sync     */
    "  .balign 128\n  b smp_ign\n"          /* 0x680 低EL A32 IRQ      */
    "  .balign 128\n  b smp_ign\n"          /* 0x700 低EL A32 FIQ      */
    "  .balign 128\n  b smp_ign\n"          /* 0x780 低EL A32 SError   */
    "  .balign 128\n"
    "smp_ign:\n"
    "  eret\n"
    "  .balign 128\n"
    "smp_irq_entry:\n"
    "  sub sp, sp, #64\n"
    "  stp x0, x1, [sp, #0]\n"
    "  stp x2, x30, [sp, #16]\n"
    "  stp x3, x4, [sp, #32]\n"
    "  bl smp_irq_handler\n"
    "  ldp x3, x4, [sp, #32]\n"
    "  ldp x2, x30, [sp, #16]\n"
    "  ldp x0, x1, [sp, #0]\n"
    "  add sp, sp, #64\n"
    "  eret\n"
);

/* ---------------- GIC / 定时器 ---------------- */
static int smp_gic_ver = 3;
void smp_set_gic_ver(int v) { smp_gic_ver = v; }

#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL
#define GICR_BASE   0x080A0000UL
#define GICR_STRIDE 0x20000UL
#define SGI_WAKE    1               /* 我们用 SGI 1 做唤醒 IPI */
#define TIMER_PPI   30              /* 物理定时器 PPI（每核私有，从核也用它做看门狗唤醒） */
#define SMP_TIMER_HZ 250            /* 从核看门狗式唤醒频率：保证任何 GIC 拓扑下都能
                                       在 1/HZ 内被唤醒认领工作（不依赖 SGI 是否可达）。 */

static void smp_gic_cpu_init(u64 cpu)
{
    if (smp_gic_ver == 3) {
        volatile u32 *rbase = (volatile u32 *)(GICR_BASE + cpu * GICR_STRIDE);
        volatile u32 *sgi   = (volatile u32 *)(GICR_BASE + cpu * GICR_STRIDE + 0x10000);
        for (volatile int i = 0; i < 1000000 && (rbase[0] & (1u << 31)); i++) { }  /* RWP */
        u32 waker = rbase[5];                    /* GICR_WAKER (0x14) */
        waker &= ~(1u << 1);                     /* 清 ProcessorSleep */
        rbase[5] = waker;
        for (volatile int i = 0; i < 1000000 && (rbase[5] & (1u << 2)); i++) { }   /* 等 ChildrenAsleep=0 */
        sgi[0x20] = 0xFFFFFFFFu;                 /* IGROUPR0：全 Group1（含 SGI1 + PPI30） */
        sgi[0x40] = (1u << SGI_WAKE) | (1u << TIMER_PPI); /* ISENABLER0：开 SGI1 + 定时器PPI */
        sgi[0x10] = (u8)0x80;                     /* IPRIORITYR(SGI1) */
        ((volatile u8 *)sgi)[0x400 + TIMER_PPI] = 0x80; /* IPRIORITYR(PPI30) */
        { u64 sre = 1; __asm__ volatile("msr ICC_SRE_EL1, %0" :: "r"(sre)); }
        __asm__ volatile("isb");
        { u64 ctl = 1; __asm__ volatile("msr ICC_CTLR_EL1, %0" :: "r"(ctl)); } /* 开非安全 CPU 接口 */
        { u64 pmr = 0xF0; __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(pmr)); }
        { u64 g1 = 1;     __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(g1)); }
        __asm__ volatile("isb");
    } else {
        /* GICv2：SGI 默认 Group0。开 CPU 接口 + 收所有优先级即可收到 SGI1。 */
        *(volatile u32 *)(GICC_BASE + 0x000) = 1;    /* GICC_CTLR.Enable */
        *(volatile u32 *)(GICC_BASE + 0x004) = 0xFF;  /* GICC_PMR */
        /* 从核定时器 PPI30：GICD_ISENABLER0(bit30) + 优先级（GICv2 的 PPI 使能是每核 banked） */
        *(volatile u32 *)(GICD_BASE + 0x100) |= (1u << TIMER_PPI);
        *(volatile u8  *)(GICD_BASE + 0x400 + TIMER_PPI) = 0x80;
    }
}

/* 从核定时器启动（每核独立物理定时器，频率 SMP_TIMER_HZ）。
   关键：必须正确重装计数（见 smp_irq_handler），否则 wfi 会立即返回 → 满速空转烧核。 */
static void smp_timer_start(void)
{
    u64 f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) f = 62500000UL;                  /* QEMU 兜底 62.5MHz */
    u64 tval = f / SMP_TIMER_HZ;
    if (tval == 0) tval = 1;
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(tval));
    __asm__ volatile("msr cntp_ctl_el0, %0"  :: "r"(1UL));  /* enable */
    __asm__ volatile("isb");
}

/* 从核 IRQ 处理：收 SGI / 定时器，计数、重装定时、唤醒工作循环。
   定时器 PPI：必须重装计数（清挂起）否则 wfi 立即返回 → 烧核。 */
void smp_irq_handler(void)
{
    u32 iar;
    if (smp_gic_ver == 3) __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    else iar = *(volatile u32 *)(GICC_BASE + 0x00C);
    u32 intid = iar & 0xFFFFFFu;

    u64 me; __asm__ volatile("mrs %0, mpidr_el1" : "=r"(me)); me &= 0xff;
    if (me < SMP_MAX) { smp_ipi_hits[me]++; smp_counter[me]++; }

    if (intid == TIMER_PPI) {                     /* 定时器：重装计数 = 清挂起 */
        u64 f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        if (f == 0) f = 62500000UL;
        u64 tval = f / SMP_TIMER_HZ;
        if (tval == 0) tval = 1;
        __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(tval));
    }

    /* GICv3：所有中断都走 Group1，必须用 EOIR1 去激活（含 SGI）；
       GICv2：SGI 走 Group0 无需 EOI，仅 PPI/SPI(id>=16) 需要。 */
    if (smp_gic_ver == 3) {
        __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(iar));
    } else if (intid >= 16) {
        *(volatile u32 *)(GICC_BASE + 0x010) = intid;
    }
}

void smp_secondary_main(u64 cpu)
{
    smp_pcpu_setup(cpu);
    smp_gic_cpu_init(cpu);
    smp_timer_start();                       /* 每核低频看门狗定时：保证任何 GIC 拓扑都能被唤醒 */
    __asm__ volatile("msr daifclr, #2");     /* 开 IRQ，准备收 SGI / 定时器 */
    if (cpu < SMP_MAX) { smp_online[cpu] = 1; smp_pcpu[cpu].online = 1; }

    for (;;) {
        __asm__ volatile("wfi");             /* 真睡（宿主 CPU ≈0%），等 SGI / 定时唤醒 */
        /* 被唤醒：若是工作 job、本核分区在范围内、且本轮还没认领过，就认领计算。
           用 done_by[cpu] 防止 active 持续为 1 期间重复计算（既省 CPU 也不虚增计数）。 */
        if (cpu < SMP_MAX && G_job.active && (int)cpu < G_job.ncores && !G_job.done_by[cpu])
            smp_do_partition((int)cpu);
    }
}

/* 主核广播 SGI（GICv2 SGIR / GICv3 ICC_SGI1R），目标 = 除自己外的所有 PE。 */
void smp_send_ipi_all(void)
{
    if (smp_gic_ver == 3) {
        u64 v = ((u64)SGI_WAKE) | ((u64)1 << 40);   /* INTID=1, IRM=1（所有其他 PE） */
        __asm__ volatile("msr ICC_SGI1R_EL1, %0" :: "r"(v));
        __asm__ volatile("isb");
    } else {
        /* GICv2 SGIR：TargetListFilter=0b01（所有其他 CPU），SGI=1。 */
        *(volatile u32 *)(GICD_BASE + 0xF00) = (1u << 24) | (u32)SGI_WAKE;
    }
}

/* ---------------- 工作分派 ---------------- */
#define SMP_WORK_SZ  2048            /* 工作缓冲（落在 .bss，已纳入 BSS 预算守卫） */
#define SMP_JOB_CHK  1               /* 分区 FNV-1a 校验和 */
#define SMP_JOB_FILL 2               /* 分区内存填充（写 pattern，验证可写并行） */

static u8  smp_work_buf[SMP_WORK_SZ];
struct smp_job G_job;                /* 全局 job 描述（主核写、从核读，靠 active 同步） */

/* 填一段伪随机数据（确定性，便于单核/多核结果互校） */
static void smp_fill_buf(void)
{
    u32 x = 0x12345678u;
    for (u32 i = 0; i < SMP_WORK_SZ; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;   /* xorshift32 */
        smp_work_buf[i] = (u8)(x & 0xFF);
    }
}

/* 单分区 FNV-1a（iters 遍叠算，放大计算量、让加速比可测） */
static u32 smp_part_fnv(const u8 *p, u64 n, u32 iters)
{
    u32 h = 2166136261u;
    for (u32 it = 0; it < iters; it++) {
        for (u64 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    }
    return h;
}

/* 把各核部分结果按固定分区序折叠成最终结果（单核/多核同一套折叠 → 可互校） */
u32 smp_combine(const u32 *part, int n)
{
    u32 a = part[0];
    for (int i = 1; i < n; i++) a = (a * 16777619u) ^ part[i];
    return a;
}

/* 从核认领分区 cpu 并计算（CPU0 也走同一函数，保证分区口径一致） */
void smp_do_partition(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX) return;
    if (!G_job.active || cpu >= G_job.ncores) return;

    u64 slice = G_job.len / (u64)G_job.ncores;
    u64 start = (u64)cpu * slice;
    u64 end   = (cpu + 1 < G_job.ncores) ? ((u64)(cpu + 1) * slice) : G_job.len;

    u64 c0, c1;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c0));
    u32 res;
    if (G_job.type == SMP_JOB_FILL) {
        for (u64 i = start; i < end; i++) smp_work_buf[i] = (u8)(i & 0xFF);
        res = smp_part_fnv(smp_work_buf + start, end - start, G_job.iters);
    } else { /* SMP_JOB_CHK */
        res = smp_part_fnv(smp_work_buf + start, end - start, G_job.iters);
    }
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c1));

    G_job.part[cpu] = res;
    G_job.cycles[cpu] = c1 - c0;
    smp_work_cycles[cpu] = c1 - c0;
    /* 顺序很关键：先写 done_by（自己那格字节，无跨核竞争），再原子递增 done。
       否则主核看到 done>=target 退出轮询后可能还没看到 done_by，会误判「从核未响应」
       而触发假 fallback（重算同一分区，结果正确但会虚报降级）。 */
    if (cpu < SMP_MAX) { G_job.done_by[cpu] = 1; smp_jobs_done[cpu]++; atomic_inc(&G_job.done); }
}

/* 主核入口：分派一次 job，自己算 partition 0，唤醒从核算其余，轮询至完成。
   ncores_limit：希望用几核（含主核）；实际参与核数会被在线核数夹住。
   返回 0 成功；结果在 G_job.part[] / G_job.done。 */
int smp_job_run(int type, u32 iters, int ncores_limit)
{
    /* 统计实际在线核数（CPU0 恒在线 + 各在线从核） */
    int online = 1;
    for (int i = 1; i < SMP_MAX; i++) if (smp_online[i]) online++;
    int target = ncores_limit;
    if (target > online) target = online;
    if (target < 1) target = 1;
    if (target > SMP_MAX) target = SMP_MAX;

    smp_fill_buf();                       /* 确定性输入 */
    G_job.active = 0;
    G_job.type   = type;
    G_job.buf    = smp_work_buf;
    G_job.len    = SMP_WORK_SZ;
    G_job.iters  = iters;
    G_job.ncores = target;
    for (int i = 0; i < SMP_MAX; i++) { G_job.part[i] = 0; G_job.cycles[i] = 0; }
    G_job.done   = 0;
    for (int i = 0; i < SMP_MAX; i++) G_job.done_by[i] = 0;
    G_job.fallback = 0;
    G_job.seq++;

    G_job.active = 1;                     /* 从核看到 active 才会认领 */
    smp_send_ipi_all();                   /* 先唤醒从核，与下面的主核计算并行 */

    smp_do_partition(0);                  /* 主核亲自算 partition 0 */

    /* 轮询完成：所有 target 个核都认领过即结束。主核用「忙轮询」而非 wfe 睡等——
       否则主核会睡到自己 10Hz 定时器(~100ms)才醒来查 done，把测速全部变成轮询延迟。
       忙轮询在微秒级内发现完成；带超大上限防呆（绝不长时间卡死 shell）。 */
    volatile int guard = 0;
    while (G_job.done < target) {
        if (++guard > 100000000) break;   /* ≈ 超时兜底，绝不死等 */
        __asm__ volatile("yield");        /* 真机/多核下让出时间片，TCG 下为空操作 */
    }
    /* 弱内存模型下保证看到各从核写好的 part[]/cycles[]（TCG 强序本身可见，
       真机 ARM 需 dsb 才能确保从核 store 对主核可见）。 */
    __asm__ volatile("dsb sy" ::: "memory");
    /* 优雅回退：若有核没唤醒（某些 GICv3 拓扑 SGI 未达、或硬件差异），主核补算
       其分区，保证结果正确、绝不丢活、绝不死等。fallback=1 供命令诚实上报降级。
       回退期间重新置 active=1，让 smp_do_partition 放行（此时不再发 IPI，无迟到从核）。 */
    G_job.active = 1;
    int missing = 0;
    for (int i = 0; i < target; i++) {
        if (!G_job.done_by[i]) { smp_do_partition(i); missing++; }
    }
    if (missing) G_job.fallback = 1;
    G_job.active = 0;
    return 0;
}

/* 单核等价计算：主核顺序把所有分区算完（不做并行分派），用于与多核结果互校。
   每个分区的算法与 smp_do_partition 完全一致 → 折叠后必等于多核结果。 */
void smp_job_compute_seq(int type, u32 iters, int ncores_limit)
{
    int online = 1;
    for (int i = 1; i < SMP_MAX; i++) if (smp_online[i]) online++;
    int target = ncores_limit;
    if (target > online) target = online;
    if (target < 1) target = 1;
    if (target > SMP_MAX) target = SMP_MAX;

    smp_fill_buf();
    G_job.active = 1;                     /* 让 smp_do_partition 放行 */
    G_job.type   = type;
    G_job.buf    = smp_work_buf;
    G_job.len    = SMP_WORK_SZ;
    G_job.iters  = iters;
    G_job.ncores = target;
    for (int i = 0; i < SMP_MAX; i++) { G_job.part[i] = 0; G_job.cycles[i] = 0; }
    G_job.done   = 0;
    for (int i = 0; i < SMP_MAX; i++) G_job.done_by[i] = 0;
    G_job.fallback = 0;
    G_job.seq++;

    for (int i = 0; i < target; i++)
        smp_do_partition(i);              /* 主核逐个分区算（与并行同口径） */
    G_job.active = 0;
}

/* 在线核数（含主核） */
int smp_online_count(void)
{
    int n = 1;
    for (int i = 1; i < SMP_MAX; i++) if (smp_online[i]) n++;
    return n;
}

/* ---------------- PSCI ---------------- */
u64 smp_ttbr0, smp_tcr, smp_mair;

static long psci_cpu_on(u64 target, u64 entry, u64 ctx)
{
    register long x0 __asm__("x0") = (long)0xC4000003UL;   /* PSCI_CPU_ON (64 位) */
    register long x1 __asm__("x1") = (long)target;
    register long x2 __asm__("x2") = (long)entry;
    register long x3 __asm__("x3") = (long)ctx;
    /* 只用 hvc：QEMU TCG 与 KVM 都经 hvc 投递 PSCI；在 EL1 执行 smc 是 UNDEFINED，
       会刷异常转储，故本版绝不调用 smc。 */
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

int smp_start(u64 *cpu_mask_out)
{
    u64 mask = 0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(smp_ttbr0));
    __asm__ volatile("mrs %0, tcr_el1"   : "=r"(smp_tcr));
    __asm__ volatile("mrs %0, mair_el1"  : "=r"(smp_mair));

    int started = 0;
    for (u64 cpu = 1; cpu < SMP_MAX; cpu++) {
        long r = psci_cpu_on(cpu, (u64)smp_secondary_entry, cpu);
        /* PSCI_CPU_ON 成功 = 0；ALREADY_ON / ON_PENDING = -3（也算拉起） */
        if (r == 0 || r == -3) { started++; mask |= (1ull << cpu); }
        else {
            /* 第一核就拿到 NOT_SUPPORTED(-1) → 本机真的没有 PSCI，标记并停止。 */
            if (started == 0 && r == -1) smp_no_conduit = 1;
            break;
        }
    }
    if (cpu_mask_out) *cpu_mask_out = mask;
    return started;
}

u64 smp_cpu_id(void)
{
    u64 v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v & 0xff;
}

int smp_wait_online(u64 cpu, int spins)
{
    for (int i = 0; i < spins; i++) {
        if (cpu < SMP_MAX && smp_online[cpu]) return 1;
        __asm__ volatile("yield");
    }
    return 0;
}
