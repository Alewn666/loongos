/* LoongOS 阶段1 - 启动汇编 + 异常向量表
 * 新增：EL2→EL1 降级（兼容 KVM/新 QEMU 从 EL2 启动）、
 *       VBAR_EL1 向量表、异常进出的完整上下文保存/恢复（31 个通用寄存器 + ELR/SPSR/ESR/FAR）*/

.section ".text.boot"
.global _start

_start:
    mov x19, x0                      /* 保存 QEMU 可能传来的 DTB 指针 */

    /* 若从 EL2 启动（KVM/部分 QEMU 配置），先降到 EL1 */
    mrs x9, CurrentEL
    lsr x9, x9, #2
    cmp x9, #2
    b.ne 1f
    mov x10, #(1 << 31)              /* HCR_EL2.RW=1：EL1 用 AArch64 */
    msr hcr_el2, x10
    mov x10, #0x3c5                  /* SPSR_EL2 = EL1h + DAIF 全屏蔽 */
    msr spsr_el2, x10
    adr x10, 1f
    msr elr_el2, x10
    eret
1:
    /* 打开 FP/NEON：CPACR_EL1.FPEN=11（位 21:20），否则任何 NEON 指令都触发
       EC=0x07 未定义异常。踩坑记录：GCC 会把 fs_init 的 1MB 大块清零自动向量化成
       `movi v0.4s`（NEON），一进 fs_init 就崩——必须在跑任何 C 代码之前打开。
       顺带把 FPSR/FPCR 清零（上电残留值不可信）。 */
    mov x10, #(3 << 20)
    msr cpacr_el1, x10
    msr fpsr, xzr
    msr fpcr, xzr
    isb

    /* 设置栈指针（.bss 里 16KB） */
    ldr x1, =_stack_top
    mov sp, x1

    /* 清零 .bss：第四十七轮改为 128B/轮（8 条 stp）。
       原实现每 8 字节「比较+str+跳回」，1.38MB 要跑 17 万次循环；
       stp 一次写 16B，循环次数降到 1 万次，开机少花几毫秒。 */
    ldr x0, =__bss_start
    ldr x1, =__bss_end
2:
    add x2, x0, #128
    cmp x2, x1
    b.hi 3f
    stp xzr, xzr, [x0]
    stp xzr, xzr, [x0, #16]
    stp xzr, xzr, [x0, #32]
    stp xzr, xzr, [x0, #48]
    stp xzr, xzr, [x0, #64]
    stp xzr, xzr, [x0, #80]
    stp xzr, xzr, [x0, #96]
    stp xzr, xzr, [x0, #112]
    add x0, x0, #128
    b 2b
3:
    cmp x0, x1
    b.ge 5f
    str xzr, [x0], #8
    b 3b
5:
    /* 装载异常向量表（16 槽 × 128 字节）
       ⚠️ ARM 架构要求：写 VBAR_EL1 后必须有 context synchronization event（ISB），
       否则不保证紧随其后的指令所产生的异常使用新向量表。第四十六轮补上。 */
    ldr x0, =vectors
    msr VBAR_EL1, x0
    isb

    mov x0, x19
    bl kernel_main

4:  wfe
    b 4b

/* ------------------------------------------------------------------ */
/* 上下文帧：x0..x30(248B) + elr(248) + spsr(256) + esr(264) + far(272)
   + q0..q31(288..799) = 800B（16 字节对齐）。
   为什么必须存全部 q0..q31（第四十七轮）：
     ARM64 只有 v8..v15 的**低 64 位**是 callee-saved，v0..v7 与 v16..v31
     全是 caller-saved，且 v8..v15 的高 64 位也不保证。异常处理 C 代码/
     被切到的另一个任务可以合法改写这些寄存器。
     不保存的后果：KDF 5 万轮里只要一次 IRQ 落到使用 NEON 的路径上，口令哈希
     就会随机算错；EL0 用户程序用 NEON 也会被内核打断后算错。这类错误极难
     复现，必须在异常入口堵死。代价是每帧 800B（IRQ 1000Hz 时约 1.6MB/s
     的进出开销），相比正确性可以接受。 */
/* ------------------------------------------------------------------ */
.macro SAVE_CTX
    sub sp, sp, #800
    stp x0, x1,  [sp, #16*0]
    stp x2, x3,  [sp, #16*1]
    stp x4, x5,  [sp, #16*2]
    stp x6, x7,  [sp, #16*3]
    stp x8, x9,  [sp, #16*4]
    stp x10, x11,[sp, #16*5]
    stp x12, x13,[sp, #16*6]
    stp x14, x15,[sp, #16*7]
    stp x16, x17,[sp, #16*8]
    stp x18, x19,[sp, #16*9]
    stp x20, x21,[sp, #16*10]
    stp x22, x23,[sp, #16*11]
    stp x24, x25,[sp, #16*12]
    stp x26, x27,[sp, #16*13]
    stp x28, x29,[sp, #16*14]
    str x30,     [sp, #16*15]
    stp q0, q1, [sp, #288]
    stp q2, q3, [sp, #320]
    stp q4, q5, [sp, #352]
    stp q6, q7, [sp, #384]
    stp q8, q9, [sp, #416]
    stp q10, q11, [sp, #448]
    stp q12, q13, [sp, #480]
    stp q14, q15, [sp, #512]
    stp q16, q17, [sp, #544]
    stp q18, q19, [sp, #576]
    stp q20, q21, [sp, #608]
    stp q22, q23, [sp, #640]
    stp q24, q25, [sp, #672]
    stp q26, q27, [sp, #704]
    stp q28, q29, [sp, #736]
    stp q30, q31, [sp, #768]
    mrs x9, elr_el1
    str x9, [sp, #248]
    mrs x9, spsr_el1
    str x9, [sp, #256]
    mrs x9, esr_el1
    str x9, [sp, #264]
    mrs x9, far_el1
    str x9, [sp, #272]
.endm

.macro RESTORE_CTX
    ldp q0, q1, [sp, #288]
    ldp q2, q3, [sp, #320]
    ldp q4, q5, [sp, #352]
    ldp q6, q7, [sp, #384]
    ldp q8, q9, [sp, #416]
    ldp q10, q11, [sp, #448]
    ldp q12, q13, [sp, #480]
    ldp q14, q15, [sp, #512]
    ldp q16, q17, [sp, #544]
    ldp q18, q19, [sp, #576]
    ldp q20, q21, [sp, #608]
    ldp q22, q23, [sp, #640]
    ldp q24, q25, [sp, #672]
    ldp q26, q27, [sp, #704]
    ldp q28, q29, [sp, #736]
    ldp q30, q31, [sp, #768]
    ldr x9, [sp, #248]
    msr elr_el1, x9
    ldr x9, [sp, #256]
    msr spsr_el1, x9
    ldp x0, x1,  [sp, #16*0]
    ldp x2, x3,  [sp, #16*1]
    ldp x4, x5,  [sp, #16*2]
    ldp x6, x7,  [sp, #16*3]
    ldp x8, x9,  [sp, #16*4]
    ldp x10, x11,[sp, #16*5]
    ldp x12, x13,[sp, #16*6]
    ldp x14, x15,[sp, #16*7]
    ldp x16, x17,[sp, #16*8]
    ldp x18, x19,[sp, #16*9]
    ldp x20, x21,[sp, #16*10]
    ldp x22, x23,[sp, #16*11]
    ldp x24, x25,[sp, #16*12]
    ldp x26, x27,[sp, #16*13]
    ldp x28, x29,[sp, #16*14]
    ldr x30,     [sp, #16*15]
    add sp, sp, #800
.endm

/* 同步异常（指令没定义/数据中止/BRK……）：传帧给 C，回来后 eret */
sync_entry:
    SAVE_CTX
    mov x0, sp
    bl handle_sync
    RESTORE_CTX
    eret

/* IRQ：handle_irq 返回"要恢复的帧指针"——可能仍是自己的帧，
   也可能被任务切换换成另一个任务栈上的帧（抢占式调度的核心钩子） */
irq_entry:
    SAVE_CTX
    mov x0, sp
    bl handle_irq
    mov sp, x0
    RESTORE_CTX
    eret

/* FIQ / SError：第四十六轮起「转储后回 shell」，不再永久停机。
   handle_fatal 会把帧里的 ELR/SPSR 改成 el1_landing / EL1h(IRQ 开)，
   这里照常恢复上下文并 eret —— 真机 ARM 上偶发一次 SError 不该让整机睡死。 */
fiq_entry:
err_entry:
    SAVE_CTX
    mov x0, sp
    bl handle_fatal
    RESTORE_CTX
    eret

/* ------------------------------------------------------------------ */
/* 向量表：16 槽（4 组 × 4 类），每槽必须从 0x80 对齐地址开始！          */
/* VBAR 硬件要求基址 128B 对齐（低 7 位 RES0）；槽间距=0x80。           */
/* ------------------------------------------------------------------ */
.macro VEC target
    .balign 128
    b \target
.endm

/* ⚠️ VBAR_EL1 要求基址 **2KB(0x800) 对齐**：低 11 位是 RES0，硬件会把它清掉。
   原来只有每个槽的 .balign 128，vectors 实际落在 0x4008027c —— QEMU TCG 不较真
   照样能跑，但真机 KVM 会把基址取整到 0x40080000，所有槽整体错位 0x27C：
   定时器中断落进"同步异常"槽，ESR 读到上电垃圾值（实测 ESR=0xdbadc0de），
   报"其他类型"然后宕机。必须显式 2KB 对齐。 */
.balign 2048
vectors:
    VEC sync_entry            /* 0x000 当前EL, SP0, 同步 */
    VEC irq_entry             /* 0x080 当前EL, SP0, IRQ */
    VEC fiq_entry             /* 0x100 FIQ */
    VEC err_entry             /* 0x180 SError */
    VEC sync_entry            /* 0x200 当前EL, SPx, 同步 */
    VEC irq_entry             /* 0x280 当前EL, SPx, IRQ */
    VEC fiq_entry
    VEC err_entry
    VEC sync_entry            /* 0x400 低EL AArch64, 同步 */
    VEC irq_entry
    VEC fiq_entry
    VEC err_entry
    VEC sync_entry            /* 0x800 低EL AArch32 */
    VEC irq_entry
    VEC fiq_entry
    VEC err_entry
