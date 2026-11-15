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

/* ==================================================================
   第五十三轮：高频中断专项压测 —— 主动污染 NEON + 逐寄存器回读校验
   ------------------------------------------------------------------
   第五十一/五十二轮诚实边界：内核抢占 timer 是 10Hz，KDF 期间未必被高频
   IRQ 打断，「800B 异常帧在真实高频中断下存 q0..q31」只证明 KDF 没被破坏，
   未做逐寄存器验证。这里用故障注入把这条补严：

     irqstress_poison()  —— IRQ 处理末尾把 q0..q31 全写成 0x5A 脏值；
     irqstress_probe()   —— 先把 32 个 q 填成各不相同的 expect[] pattern，
                            硬件 timer 提到 1000Hz，每来一次 IRQ 就逐寄存器
                            回读低/高 64bit lane 比对，累计 mismatch。

   若 SAVE_CTX/RESTORE_CTX 漏存或偏移错位任何一个 q，投毒值就会漏回来，
   mismatch > 0。calibration 直接调用 irqstress_poison() 弄脏全部 32 个 q，
   要求逐寄存器比对报满 64 个失配，验证「投毒生效 + 比对逻辑有效」，
   杜绝判据恒真。全程只读 expect[]，只碰 caller-saved x0..x18 与已保存的
   x19/x20/x29/x30；被 IRQ 打断时 SAVE_CTX 会把这些 x 与 q 一起保存恢复。
   ================================================================== */

.macro QPOISON simdop
    movi \simdop, #0x5A
.endm
.macro QLOAD laneop, dreg, off, base
    ldp x9, x10, [\base, #\off]
    fmov \dreg, x9
    ins \laneop, x10
.endm
.macro QCHECK laneop, dreg, off, base, bad
    fmov x9, \dreg
    umov x10, \laneop
    ldp x17, x18, [\base, #\off]
    cmp x9, x17
    cset x16, ne
    add \bad, \bad, x16
    cmp x10, x18
    cset x16, ne
    add \bad, \bad, x16
.endm

.global irqstress_poison
irqstress_poison:
    QPOISON v0.16b
    QPOISON v1.16b
    QPOISON v2.16b
    QPOISON v3.16b
    QPOISON v4.16b
    QPOISON v5.16b
    QPOISON v6.16b
    QPOISON v7.16b
    QPOISON v8.16b
    QPOISON v9.16b
    QPOISON v10.16b
    QPOISON v11.16b
    QPOISON v12.16b
    QPOISON v13.16b
    QPOISON v14.16b
    QPOISON v15.16b
    QPOISON v16.16b
    QPOISON v17.16b
    QPOISON v18.16b
    QPOISON v19.16b
    QPOISON v20.16b
    QPOISON v21.16b
    QPOISON v22.16b
    QPOISON v23.16b
    QPOISON v24.16b
    QPOISON v25.16b
    QPOISON v26.16b
    QPOISON v27.16b
    QPOISON v28.16b
    QPOISON v29.16b
    QPOISON v30.16b
    QPOISON v31.16b
    ret

/* x0=want_irqs  x1=&g_timer_irqs  x2=&irq_expect[0]  x3=&irq_out[0]
   out: [0]=rounds [1]=q_mismatch [2]=irq_start_lo [3]=irq_end_lo
        [4]=calibration（调用真实投毒后应为 64 个 lane 失配） [5]=timeout */
.global irqstress_probe
irqstress_probe:
    stp x29, x30, [sp, #-32]!
    stp x19, x20, [sp, #16]
    mov x20, #0                 /* timeout 标志 */
    str wzr, [x3, #20]         /* out[5] = 0 */

    /* ① 32 个 q 填成各不相同的 expect[] pattern */
    QLOAD v0.d[1], d0, 0, x2
    QLOAD v1.d[1], d1, 16, x2
    QLOAD v2.d[1], d2, 32, x2
    QLOAD v3.d[1], d3, 48, x2
    QLOAD v4.d[1], d4, 64, x2
    QLOAD v5.d[1], d5, 80, x2
    QLOAD v6.d[1], d6, 96, x2
    QLOAD v7.d[1], d7, 112, x2
    QLOAD v8.d[1], d8, 128, x2
    QLOAD v9.d[1], d9, 144, x2
    QLOAD v10.d[1], d10, 160, x2
    QLOAD v11.d[1], d11, 176, x2
    QLOAD v12.d[1], d12, 192, x2
    QLOAD v13.d[1], d13, 208, x2
    QLOAD v14.d[1], d14, 224, x2
    QLOAD v15.d[1], d15, 240, x2
    QLOAD v16.d[1], d16, 256, x2
    QLOAD v17.d[1], d17, 272, x2
    QLOAD v18.d[1], d18, 288, x2
    QLOAD v19.d[1], d19, 304, x2
    QLOAD v20.d[1], d20, 320, x2
    QLOAD v21.d[1], d21, 336, x2
    QLOAD v22.d[1], d22, 352, x2
    QLOAD v23.d[1], d23, 368, x2
    QLOAD v24.d[1], d24, 384, x2
    QLOAD v25.d[1], d25, 400, x2
    QLOAD v26.d[1], d26, 416, x2
    QLOAD v27.d[1], d27, 432, x2
    QLOAD v28.d[1], d28, 448, x2
    QLOAD v29.d[1], d29, 464, x2
    QLOAD v30.d[1], d30, 480, x2
    QLOAD v31.d[1], d31, 496, x2

    /* ② calibration：调用**真实投毒函数**弄脏全部 32 个 q，逐寄存器比对
          必须报 64 个失配（32 寄存器 × 2 个 64bit lane）。这一步同时验证
          「比对逻辑有效」与「irqstress_poison 确实改写了每一个 q」，
          杜绝后面的 PASS 只是因为压根没污染。 */
    bl irqstress_poison
    ldr x30, [sp, #8]          /* bl 覆盖了返回地址，从栈上取回 */
    mov x6, #0
    QCHECK v0.d[1], d0, 0, x2, x6
    QCHECK v1.d[1], d1, 16, x2, x6
    QCHECK v2.d[1], d2, 32, x2, x6
    QCHECK v3.d[1], d3, 48, x2, x6
    QCHECK v4.d[1], d4, 64, x2, x6
    QCHECK v5.d[1], d5, 80, x2, x6
    QCHECK v6.d[1], d6, 96, x2, x6
    QCHECK v7.d[1], d7, 112, x2, x6
    QCHECK v8.d[1], d8, 128, x2, x6
    QCHECK v9.d[1], d9, 144, x2, x6
    QCHECK v10.d[1], d10, 160, x2, x6
    QCHECK v11.d[1], d11, 176, x2, x6
    QCHECK v12.d[1], d12, 192, x2, x6
    QCHECK v13.d[1], d13, 208, x2, x6
    QCHECK v14.d[1], d14, 224, x2, x6
    QCHECK v15.d[1], d15, 240, x2, x6
    QCHECK v16.d[1], d16, 256, x2, x6
    QCHECK v17.d[1], d17, 272, x2, x6
    QCHECK v18.d[1], d18, 288, x2, x6
    QCHECK v19.d[1], d19, 304, x2, x6
    QCHECK v20.d[1], d20, 320, x2, x6
    QCHECK v21.d[1], d21, 336, x2, x6
    QCHECK v22.d[1], d22, 352, x2, x6
    QCHECK v23.d[1], d23, 368, x2, x6
    QCHECK v24.d[1], d24, 384, x2, x6
    QCHECK v25.d[1], d25, 400, x2, x6
    QCHECK v26.d[1], d26, 416, x2, x6
    QCHECK v27.d[1], d27, 432, x2, x6
    QCHECK v28.d[1], d28, 448, x2, x6
    QCHECK v29.d[1], d29, 464, x2, x6
    QCHECK v30.d[1], d30, 480, x2, x6
    QCHECK v31.d[1], d31, 496, x2, x6
    str w6, [x3, #16]          /* out[4] = 64 才说明投毒与比对都生效 */
    /* 重新填回 pattern，进入主循环 */
    QLOAD v0.d[1], d0, 0, x2
    QLOAD v1.d[1], d1, 16, x2
    QLOAD v2.d[1], d2, 32, x2
    QLOAD v3.d[1], d3, 48, x2
    QLOAD v4.d[1], d4, 64, x2
    QLOAD v5.d[1], d5, 80, x2
    QLOAD v6.d[1], d6, 96, x2
    QLOAD v7.d[1], d7, 112, x2
    QLOAD v8.d[1], d8, 128, x2
    QLOAD v9.d[1], d9, 144, x2
    QLOAD v10.d[1], d10, 160, x2
    QLOAD v11.d[1], d11, 176, x2
    QLOAD v12.d[1], d12, 192, x2
    QLOAD v13.d[1], d13, 208, x2
    QLOAD v14.d[1], d14, 224, x2
    QLOAD v15.d[1], d15, 240, x2
    QLOAD v16.d[1], d16, 256, x2
    QLOAD v17.d[1], d17, 272, x2
    QLOAD v18.d[1], d18, 288, x2
    QLOAD v19.d[1], d19, 304, x2
    QLOAD v20.d[1], d20, 320, x2
    QLOAD v21.d[1], d21, 336, x2
    QLOAD v22.d[1], d22, 352, x2
    QLOAD v23.d[1], d23, 368, x2
    QLOAD v24.d[1], d24, 384, x2
    QLOAD v25.d[1], d25, 400, x2
    QLOAD v26.d[1], d26, 416, x2
    QLOAD v27.d[1], d27, 432, x2
    QLOAD v28.d[1], d28, 448, x2
    QLOAD v29.d[1], d29, 464, x2
    QLOAD v30.d[1], d30, 480, x2
    QLOAD v31.d[1], d31, 496, x2

    /* ③ 主循环：每来一次真实 TIMER IRQ 就做一轮 32×2 逐寄存器比对 */
    mov x6, #0                 /* mismatch 累计 */
    mov x7, #0                 /* rounds */
    mov x19, #0                /* 轮询计数（防 IRQ 不来时挂死） */
    mov x8, #0x1000
    lsl x8, x8, #16            /* x8 = 2^28 轮询上限 */
    ldr x4, [x1]               /* 起始 IRQ 计数 */
    str w4, [x3, #8]           /* out[2] */
    add x5, x4, x0             /* 目标 IRQ 计数 */
1:
    ldr x15, [x1]
    cmp x15, x4
    b.ne 2f                    /* IRQ 计数变了 → 做检查 */
    add x19, x19, #1
    cmp x19, x8
    b.ge 4f                    /* 超时：不挂死，明确失败 */
    b 1b
2:
    mov x4, x15
    QCHECK v0.d[1], d0, 0, x2, x6
    QCHECK v1.d[1], d1, 16, x2, x6
    QCHECK v2.d[1], d2, 32, x2, x6
    QCHECK v3.d[1], d3, 48, x2, x6
    QCHECK v4.d[1], d4, 64, x2, x6
    QCHECK v5.d[1], d5, 80, x2, x6
    QCHECK v6.d[1], d6, 96, x2, x6
    QCHECK v7.d[1], d7, 112, x2, x6
    QCHECK v8.d[1], d8, 128, x2, x6
    QCHECK v9.d[1], d9, 144, x2, x6
    QCHECK v10.d[1], d10, 160, x2, x6
    QCHECK v11.d[1], d11, 176, x2, x6
    QCHECK v12.d[1], d12, 192, x2, x6
    QCHECK v13.d[1], d13, 208, x2, x6
    QCHECK v14.d[1], d14, 224, x2, x6
    QCHECK v15.d[1], d15, 240, x2, x6
    QCHECK v16.d[1], d16, 256, x2, x6
    QCHECK v17.d[1], d17, 272, x2, x6
    QCHECK v18.d[1], d18, 288, x2, x6
    QCHECK v19.d[1], d19, 304, x2, x6
    QCHECK v20.d[1], d20, 320, x2, x6
    QCHECK v21.d[1], d21, 336, x2, x6
    QCHECK v22.d[1], d22, 352, x2, x6
    QCHECK v23.d[1], d23, 368, x2, x6
    QCHECK v24.d[1], d24, 384, x2, x6
    QCHECK v25.d[1], d25, 400, x2, x6
    QCHECK v26.d[1], d26, 416, x2, x6
    QCHECK v27.d[1], d27, 432, x2, x6
    QCHECK v28.d[1], d28, 448, x2, x6
    QCHECK v29.d[1], d29, 464, x2, x6
    QCHECK v30.d[1], d30, 480, x2, x6
    QCHECK v31.d[1], d31, 496, x2, x6
    add x7, x7, #1
    cmp x15, x5
    b.lt 1b
    b 5f
4:
    mov x20, #1
5:
    str w7, [x3, #0]           /* out[0] = rounds */
    str w6, [x3, #4]           /* out[1] = q_mismatch */
    str w4, [x3, #12]          /* out[3] = irq_end */
    str w20, [x3, #20]         /* out[5] = timeout */
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

