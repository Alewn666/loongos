/* LoongOS x86_64 · IDT 中断入口 stub
 * ----------------------------------------------------------------------------
 * 设计（仿 Linux arch/x86/entry/entry_64.S 的 irq_entries 思路，精简版）：
 *   - 每个向量一个 stub，压入「占位错误码 + 向量号」后跳到 irq_common；
 *   - irq_common 保存所有通用寄存器（x86 中断不自动保存 GPR），
 *     调 platform_irq_dispatch(vec) 做 C 侧分派 + 发 EOI，
 *     恢复寄存器 → 丢掉 vec/err → iretq。
 *   - CPU 在 ring0 同特权级中断时自动压 SS/RSP/RFLAGS/CS/RIP（不切栈）；
 *     为保持栈 16 字节对齐，保存完 GPR 后把 rsp 对齐再 call（用 rbx 暂存原值）。
 *
 * 只实现真正用到的向量：0x20(PIT)、0x21(键盘)、0x2C(鼠标 IRQ12)，
 * 其余用一个统一 stub 打日志。异常向量（0..31）也走统一 stub。
 */
.section ".text", "ax"
.code64

.macro STUB vec
    .global isr_stub_\vec
    isr_stub_\vec:
        pushq $0            /* 占位错误码，让栈帧形状统一 */
        pushq $\vec         /* 向量号 */
        jmp irq_common
.endm

STUB 0x20                  /* IRQ0  PIT 定时器 */
STUB 0x21                  /* IRQ1  i8042 键盘 */
STUB 0x2C                  /* IRQ12 i8042 鼠标 */

/* 其余所有向量共用一个 stub，vec = 0xFF */
.global isr_stub_other
isr_stub_other:
    pushq $0
    pushq $0xFF
    jmp irq_common

/* 供 C 建 IDT 用的 stub 地址表入口 */
.global isr_stub_0x20_addr
.global isr_stub_0x21_addr
.global isr_stub_0x2C_addr
.global isr_stub_other_addr
.section ".rodata", "a"
.align 8
isr_stub_0x20_addr:   .quad isr_stub_0x20
isr_stub_0x21_addr:   .quad isr_stub_0x21
isr_stub_0x2C_addr:   .quad isr_stub_0x2C
isr_stub_other_addr:  .quad isr_stub_other

.section ".text", "ax"
.code64
.global irq_common
irq_common:
    /* 保存 GPR（15 个 = 120 字节）。保存后 vec 位于 120(%rsp)，err 在 128(%rsp)。 */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq 120(%rsp), %rdi          /* vec → C 第一个参数 */
    movq %rsp, %rbx               /* 暂存原始 rsp（rbx 已入栈，可安全覆写） */
    andq $-16, %rsp               /* SysV ABI：call 前 rsp 16 字节对齐 */
    call platform_irq_dispatch
    movq %rbx, %rsp               /* 恢复原始栈帧 */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp                /* 丢弃 vec + 占位 err */
    iretq

.section .note.GNU-stack, "", @progbits
