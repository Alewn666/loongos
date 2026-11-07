/* LoongOS 阶段9 —— SMP（多核）
 *
 * 已实现：
 *   1. PSCI CPU_ON 拉起从核（smc，回退 hvc）
 *   2. per-CPU 私有状态（TPIDR_EL1 指向，Linux this_cpu() 的简化版）
 *   3. 控制台自旋锁（console_lock）
 *   4. 每核 GIC redistributor + 每核定时器 → 从核用 wfi 睡等（不再忙等）
 *
 * 刻意保守：内核主逻辑（shell/fs/网络）仍只在 CPU0 跑，从核只跑定时器计数。
 * 下一步才是 IPI、锁住 fs/net、per-CPU 调度。
 *
 * ⚠️ 两条硬约束（踩过）：
 *   - cortex-a57 没有 LSE 原子指令，且我们 -nostdlib（没有 libgcc 的 __aarch64_ldadd），
 *     原子读改写必须手写 ldxr/stxr。
 *   - MMU 未开时内存是 Device 属性，ldaxr/stxr 不可用 → 锁用 smp_locks_on 门住。
 */

typedef unsigned long u64;
typedef unsigned int  u32;
typedef unsigned char u8;

#define SMP_MAX 4

/* 控制台锁 + 启用开关 */
volatile int console_lock = 0;
volatile int smp_locks_on = 0;

/* ---------------- per-CPU 私有状态 ---------------- */
struct percpu {
    u64 cpu_id;
    u64 tick;          /* 本核 tick（CPU0 由 shell 循环推、从核由定时器中断推） */
    u64 irq_depth;
    u64 online;
    u64 stack_top;
};
struct percpu smp_pcpu[SMP_MAX];

u64 smp_stacks[SMP_MAX][512] __attribute__((aligned(16)));   /* 每核 4KB 栈 */

volatile u64 smp_online[SMP_MAX];
volatile u64 smp_counter[SMP_MAX];

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

void smp_tick_inc(void)
{
    struct percpu *p = this_cpu();
    if (!p) return;
    u64 old, nv; int fail;
    do {
        __asm__ volatile("ldxr %0, [%1]" : "=r"(old) : "r"(&p->tick) : "memory");
        nv = old + 1;
        __asm__ volatile("stxr %w0, %2, [%1]" : "=&r"(fail) : "r"(&p->tick), "r"(nv) : "memory");
    } while (fail);
}

u64 smp_pcpu_tick(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX) return 0;
    return *(volatile u64 *)&smp_pcpu[cpu].tick;
}

/* ---------------- 自旋锁 ---------------- */
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
 * 2026-11-07 修复：原版把 16 条 b 连写在前 64 字节里，误以为「第 6 条 = 0x280」。
 * 反汇编实测 IRQ 分支落在 +0x14；而「当前 EL / SPx / IRQ」是 0x280，那里当时是
 * nop 填充 → 从核定时期中断进不了 smp_irq_handler，tick 恒为 0、wfi 空转。
 * 现按 boot.S 的 VEC 宏风格，每槽 .balign 128，VBAR 2KB 对齐（真机 KVM 会取整）。 */
__asm__(
    ".text\n"
    ".balign 2048\n"
    ".global smp_vectors\n"
    "smp_vectors:\n"
    "  .balign 128\n  b smp_ign\n"          /* 0x000 当前EL SP0 Sync   */
    "  .balign 128\n  b smp_ign\n"          /* 0x080 当前EL SP0 IRQ    */
    "  .balign 128\n  b smp_ign\n"          /* 0x100 当前EL SP0 FIQ    */
    "  .balign 128\n  b smp_ign\n"          /* 0x180 当前EL SP0 SError */
    "  .balign 128\n  b smp_ign\n"          /* 0x200 当前EL SPx Sync   */
    "  .balign 128\n  b smp_irq_entry\n"    /* 0x280 当前EL SPx IRQ ← 从核定时器 */
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
volatile int smp_no_conduit = 0;   /* 1 = 本机无可用 PSCI conduit（见 smp_start）*/
void smp_set_gic_ver(int v) { smp_gic_ver = v; }

#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL
#define GICR_BASE   0x080A0000UL
#define GICR_STRIDE 0x20000UL
#define TIMER_PPI   30

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
        sgi[0x20] = 0xFFFFFFFFu;                 /* IGROUPR0：全 Group1 */
        ((volatile u8 *)sgi)[0x400 + TIMER_PPI] = 0x80;
        sgi[0x40] = (1u << TIMER_PPI);           /* ISENABLER0：开 PPI30 */
        { u64 sre = 1; __asm__ volatile("msr ICC_SRE_EL1, %0" :: "r"(sre)); }
        __asm__ volatile("isb");
        { u64 pmr = 0xF0; __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(pmr)); }
        { u64 g1 = 1;     __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(g1)); }
        __asm__ volatile("isb");
    } else {
        *(volatile u32 *)(GICD_BASE + 0x400 + TIMER_PPI) = 0x80;
        *(volatile u32 *)(GICD_BASE + 0x100 + 4 * (TIMER_PPI / 32)) = (1u << (TIMER_PPI % 32));
        *(volatile u32 *)(GICC_BASE + 0x000) = 1;
        *(volatile u32 *)(GICC_BASE + 0x004) = 0xFF;
    }
}

static void smp_timer_start(void)
{
    u64 f, tval, ctl = 1;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    tval = f / 100;
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(tval));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(ctl));
    __asm__ volatile("isb");
}

void smp_irq_handler(void)
{
    u64 iar = 0;
    if (smp_gic_ver == 3) __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    else iar = *(volatile u32 *)(GICC_BASE + 0x00C);
    u32 intid = (u32)(iar & 0xFFFFFFu);
    if (intid == TIMER_PPI) {
        smp_timer_start();
        smp_tick_inc();
    }
    if (smp_gic_ver == 3) __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(iar));
    else *(volatile u32 *)(GICC_BASE + 0x010) = intid;
}

void smp_secondary_main(u64 cpu)
{
    smp_pcpu_setup(cpu);
    smp_gic_cpu_init(cpu);
    smp_timer_start();
    /* ⚠️ 必须开 IRQ：PSCI 交给从核时 DAIF 是全屏蔽的。不开的话定时器到期只会把 IRQ
       挂起而进不了处理程序 → wfi 每次立刻返回 → 满速空转（实测宿主 vCPU 烧掉 28s CPU） */
    __asm__ volatile("msr daifclr, #2");
    if (cpu < SMP_MAX) { smp_online[cpu] = 1; smp_pcpu[cpu].online = 1; }
    for (;;) {
        __asm__ volatile("wfi");       /* 睡到自己的定时器中断 */
        if (cpu < SMP_MAX) smp_counter[cpu]++;
    }
}

/* ---------------- PSCI ---------------- */
u64 smp_ttbr0, smp_tcr, smp_mair;

static long psci_cpu_on(u64 target, u64 entry, u64 ctx)
{
    register long x0 __asm__("x0") = (long)0xC4000003UL;   /* PSCI_CPU_ON (64 位) */
    register long x1 __asm__("x1") = (long)target;
    register long x2 __asm__("x2") = (long)entry;
    register long x3 __asm__("x3") = (long)ctx;
    __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    if ((u64)x0 == 0xFFFFFFFFFFFFFFFFULL) {
        x0 = (long)0xC4000003UL; x1 = (long)target; x2 = (long)entry; x3 = (long)ctx;
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    }
    return x0;
}

extern void smp_secondary_entry(void);

int smp_start(u64 *cpu_mask_out)
{
    u64 mask = 0;
    /* 2026-11-07：PSCI conduit 探测。
       QEMU `-machine virt` 默认既无 EL3 也无 EL2，此时在 EL1 执行 smc 是 UNDEFINED，
       原版会触发内核异常转储后返回函数 ID（≠ -1，所以 hvc 回退也不触发）→ 刷一屏异常、
       从核 0 个。ID_AA64PFR0_EL1[15:12]=EL3、[11:8]=EL2；两者皆 0 就明确放弃并说明，
       不再制造异常。加 -machine virt,virtualization=on（EL2 可用）即可启用多核。 */
    {
        u64 pfr0, cur;
        __asm__ volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(pfr0));
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur));
        int el3 = (int)((pfr0 >> 12) & 0xF);
        int el2 = (int)((pfr0 >> 8)  & 0xF);
        if (((cur >> 2) & 3) == 1 && !el3 && !el2) {
            if (cpu_mask_out) *cpu_mask_out = 0;
            smp_no_conduit = 1;
            return 0;                 /* 交给调用方打印一行说明，不刷异常 */
        }
    }
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(smp_ttbr0));
    __asm__ volatile("mrs %0, tcr_el1"   : "=r"(smp_tcr));
    __asm__ volatile("mrs %0, mair_el1"  : "=r"(smp_mair));

    int started = 0;
    for (u64 cpu = 1; cpu < SMP_MAX; cpu++) {
        long r = psci_cpu_on(cpu, (u64)smp_secondary_entry, cpu);
        if (r == 0 || r == -3) { started++; mask |= (1ull << cpu); }
        else break;
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
