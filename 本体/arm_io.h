/* LoongOS 第四十六轮：ARM64 弱内存序统一屏障层
 * ============================================================================
 * 为什么需要这一层
 * ----------------
 * ARM64 是**弱内存序**架构：CPU 写的普通内存（DMA 描述符 / 数据缓冲）与 MMIO
 * 写（门铃、notify 寄存器）之间的先后顺序，硬件不自动保证；x86 的强序 TSO 会
 * 替你保证，所以「在 x86 上能跑」的代码搬到真机 ARM 上会偶发「设备看不到描述符
 * → 设备不响应 → 驱动忙等到超时 → 表现为用着用着死机」。
 *
 * 项目此前每个驱动各自写一份 `__asm__ volatile("dsb sy")`（共 17 处），语义全是
 * 最重的「全系统屏障」。本头文件把屏障按**语义**分档，并提供 cache 维护原语，
 * 让后续驱动改造有唯一事实来源：
 *
 *   sys_dsb()   = dsb sy  低频强序：设备复位、PCI 配置空间、MMU/TLB、跨核同步
 *   mmio_wmb()  = dsb st  高频：CPU 写完 DMA 描述符/avail → 写 MMIO 通知设备前
 *                          （只等 store 完成，不等 load，比 dsb sy 轻）
 *   dma_wmb()   = dmb st  CPU 写 DMA 缓冲（暂不通知设备）之间的顺序
 *   dma_rmb()   = dmb ld  设备写回 used.idx / RX 数据 → CPU 读之前
 *
 * ⚠️ 关于 cache 维护（诚实边界，别乱铺）
 * ------------------------------------
 *   - QEMU TCG：dc 指令是 no-op，加了不影响功能；
 *   - KVM / 带 CCI 的 ARM 服务器：DMA 是**硬件 cache-coherent**，CPU 与设备看同一份
 *     数据，软件 clean/invalidate 不是必需，滥用反而可能丢掉刚写入的数据；
 *   - 裸机非一致性 SoC（部分 ARM 开发板）：必须做，否则设备看不到 CPU 脏 cache 里的
 *     描述符，正是「偶发卡死」的根因之一。
 *   因此下面只**提供原语**，由具体驱动按平台决定是否调用，不统一铺到所有 virtio
 *   路径。nvme.c 已有等价实现（nv_clean_buf / nv_inv_buf），可作为非一致性平台的
 *   落地范例。本轮优化重点放在「异常/中断不再停机 + 架构同步指令 + 对齐安全」，
 *   避免在 cache-coherent 平台上引入数据损坏。
 */
#ifndef LOONGOS_ARM_IO_H
#define LOONGOS_ARM_IO_H

/* ---- 基础屏障（全部带 memory clobber，阻止编译器重排） ---- */
static inline void arm_isb(void)     { __asm__ volatile("isb"    ::: "memory"); }
static inline void arm_dsb_sy(void)  { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void arm_dsb_st(void)  { __asm__ volatile("dsb st" ::: "memory"); }
static inline void arm_dsb_ld(void)  { __asm__ volatile("dsb ld" ::: "memory"); }
static inline void arm_dmb_sy(void)  { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void arm_dmb_st(void)  { __asm__ volatile("dmb st" ::: "memory"); }
static inline void arm_dmb_ld(void)  { __asm__ volatile("dmb ld" ::: "memory"); }

/* 语义化别名：驱动里写这些，一眼看出意图与强弱 */
#define sys_dsb()    arm_dsb_sy()
#define mmio_wmb()   arm_dsb_st()
#define dma_wmb()    arm_dmb_st()
#define dma_rmb()    arm_dmb_ld()

/* ---- 低功耗 / 调度提示 ---- */
static inline void arm_wfi(void)   { __asm__ volatile("wfi"); }
static inline void arm_wfe(void)   { __asm__ volatile("wfe"); }
static inline void arm_sev(void)   { __asm__ volatile("sev"); }
static inline void arm_yield(void) { __asm__ volatile("yield"); }

/* ---- Cache 维护原语（按 64B cacheline）----
 * 仅在上述「非一致性裸机 SoC」场景由驱动显式调用；不调用时不生成任何代码。 */
#define ARM_CACHELINE 64ULL
static inline void arm_dc_cvac(u64 a)  { __asm__ volatile("dc cvac, %0"  :: "r"(a) : "memory"); }
static inline void arm_dc_civac(u64 a) { __asm__ volatile("dc civac, %0" :: "r"(a) : "memory"); }
static inline void arm_dc_ivac(u64 a)  { __asm__ volatile("dc ivac, %0"  :: "r"(a) : "memory"); }

/* CPU 写完 DMA 缓冲 → 设备要读：把脏行清回内存 */
static inline void arm_dma_clean(u64 addr, u64 len)
{
    u64 a = addr & ~(ARM_CACHELINE - 1), e = addr + len;
    for (; a < e; a += ARM_CACHELINE) arm_dc_cvac(a);
    arm_dsb_st();
}
/* 设备写完 DMA 缓冲 → CPU 要读：先失效可能残留的旧行 */
static inline void arm_dma_invalidate(u64 addr, u64 len)
{
    u64 a = addr & ~(ARM_CACHELINE - 1), e = addr + len;
    for (; a < e; a += ARM_CACHELINE) arm_dc_ivac(a);
    arm_dsb_ld();
}

#endif /* LOONGOS_ARM_IO_H */
