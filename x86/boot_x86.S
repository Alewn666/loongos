/* LoongOS x86_64 · 启动汇编（Multiboot2 → 长模式）
 * ----------------------------------------------------------------------------
 * 进入状态（Multiboot2 spec / QEMU -kernel 与 GRUB 一致）：
 *   - IA-32 保护模式，CR0.PE=1 CR0.PG=0，A20 已开，中断应关闭
 *   - EAX = 0x36d76289（MB2 magic），EBX = MBI 物理地址
 *   - CS/DS/ES/SS = flat 32 位段，但 GDT 内容由 bootloader 提供，不可长期依赖
 *   - ESP 未定义 → 必须自己设栈
 *
 * 步骤：自建 2MB 大页 identity 映射低 1GB → 开 PAE/LME/PG → 远跳 64 位代码段
 *       → 装载 0x10 数据段 → 清 .bss → 传 (magic, mbi) 给 kernel_x86_main。
 *
 * 关键坑（对齐 OSDev / Linux arch/x86/boot/compressed/head_64.S）：
 *   1) 长模式下必须先开 PAE(CR4.5) 再写 EFER.LME，最后 CR0.PG，顺序不能反；
 *   2) 开 PG 后只是「兼容模式」，必须一次 far ljmp 到 L=1 的代码段才真正进 64 位；
 *   3) 页表/GDT/早期栈放在独立 .bootpkt 段（nobits），绝不能落在会被清零的 .bss；
 *   4) 用 .byte 手工编码 far jmp，避免 gas 在 .code32 下把 offset 编成 16 位。
 */

/* ==================== Multiboot2 header ==================== */
.section ".multiboot", "ax"
.align 8
mb2_hdr:
    .long 0xE85250D6                 /* magic */
    .long 0                          /* architecture = i386（32 位入口，最兼容） */
    .long mb2_end - mb2_hdr          /* header length */
    .long -(0xE85250D6 + 0 + (mb2_end - mb2_hdr))  /* checksum */

    /* Multiboot2 framebuffer request tag (type=5, size=20)：
       请 GRUB/固件设置 800x600x32 图形模式，把线性帧缓冲信息通过 MBI tag 8
       传进内核。这是 x86 标准做法（VBE / EFI GOP 都走这条），不依赖 QEMU
       私有的 fw_cfg ramfb（后者还要求 fw_cfg 带 DMA，x86 pc 机默认没有）。 */
    .align 8
    .short 5                         /* type = MB2_TAG_FRAMEBUFFER */
    .short 0                         /* flags = 0（optional） */
    .long 20                         /* size */
    .long 800                        /* width */
    .long 600                        /* height */
    .long 32                         /* depth */

    .align 8
    .short 0                         /* end tag type */
    .short 0                         /* flags */
    .long 8                          /* size */
mb2_end:

/* ==================== 32 位入口 ==================== */
.section ".text.boot", "ax"
.code32
.global _start
_start:
    cli
    cld
    movl $early_stack_top, %esp

    /* 32 位模式下写 edi/esi 会把对应 64 位寄存器高半部清零（x86-64 架构特性），
       因此进 64 位后 rdi/rsi 直接就是零扩展后的 magic / mbi，可作为 C 前两个参数。 */
    movl %eax, %edi                  /* rdi = MB2 magic */
    movl %ebx, %esi                  /* rsi = MBI 物理地址 */

    /* ---- 建页表：identity 映射低 4GB ----
       PML4[0]→PDPT[0..3]→4 个 PD，每个 PD 512 项 2MB 大页 = 1GB，共 4GB。
       必须覆盖 4GB：VBE/GOP 的线性帧缓冲常在 0xE0000000 附近，只映射 1GB
       会一写就 #PF。4 个 PD 页在内存里连续，于是 pd[j] 对应物理 j*2MB，
       一个 2048 次循环即可填满。 */
    movl $pml4, %eax
    movl $pdpt, %ebx
    orl  $3, %ebx                    /* Present | RW */
    movl %ebx, (%eax)

    /* PDPT[0..3] = &pd + i*4096 | 3 */
    xorl %ecx, %ecx
fill_pdpt:
    movl %ecx, %ebx
    shll $12, %ebx                   /* i * 4096 */
    addl $pd, %ebx
    orl  $3, %ebx
    movl %ebx, pdpt(,%ecx,8)
    incl %ecx
    cmpl $4, %ecx
    jb   fill_pdpt

    /* 2048 个 PDE：pd[j] = (j*2MB) | 0x83 */
    xorl %ecx, %ecx
fill_pd:
    movl %ecx, %ebx
    shll $21, %ebx                   /* j * 2MB */
    orl  $0x83, %ebx                 /* Present | RW | PS(2MB 大页) */
    movl %ebx, pd(,%ecx,8)
    incl %ecx
    cmpl $2048, %ecx
    jb   fill_pd

    /* ---- 装载我们自己的 GDT（含 64 位代码段）---- */
    lgdt gdt32_desc

    /* ---- CR4.PAE ---- */
    movl %cr4, %eax
    btsl $5, %eax
    movl %eax, %cr4

    /* ---- CR3 = PML4 ---- */
    movl $pml4, %eax
    movl %eax, %cr3

    /* ---- EFER.LME (MSR 0xC0000080 bit8) ---- */
    movl $0xC0000080, %ecx
    rdmsr
    btsl $8, %eax
    wrmsr

    /* ---- CR0: PG | WP | NE | PE ---- */
    movl %cr0, %eax
    btsl $31, %eax                   /* PG 开启分页 → 进入兼容模式 */
    btsl $16, %eax                   /* WP */
    btsl $5,  %eax                   /* NE */
    btsl $0,  %eax                   /* PE */
    movl %eax, %cr0

    /* ---- far jmp 到 64 位代码段 0x08：手工编码 EA off32 sel16 ---- */
    .byte 0xEA
    .long long_start
    .word 0x08

/* ==================== 64 位入口 ==================== */
.code64
long_start:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $early_stack_top, %rsp

    /* 清 .bss：只清这一段，.bootpkt（页表/GDT/栈）不动。
       用 r8/r9 作指针，避免破坏 rdi/rsi 里的 magic/mbi。 */
    leaq __bss_start(%rip), %r8
    leaq __bss_end(%rip), %r9
    xorq %rax, %rax
clr_bss:
    cmpq %r9, %r8
    jae  clr_done
    movq %rax, (%r8)
    addq $8, %r8
    jmp  clr_bss
clr_done:

    /* SysV AMD64 ABI：rdi=arg0(magic) rsi=arg1(mbi) */
    call kernel_x86_main

    /* 正常不返回；返回就停机 */
1:  cli
    hlt
    jmp 1b

/* ==================== 数据（不进 .bss）==================== */
.section ".rodata.gdt", "a"
.align 16
gdt:
    .quad 0x0000000000000000         /* 0x00 null */
    .quad 0x00AF9A000000FFFF         /* 0x08 code64: P DPL0 S Type=A G=1 L=1 */
    .quad 0x00AF92000000FFFF         /* 0x10 data64: P DPL0 S Type=2 G=1 */
gdt_end:

.align 16
/* 32 位 lgdt 读 6 字节：limit(2) + base(4) */
gdt32_desc:
    .word gdt_end - gdt - 1
    .long gdt

/* ==================== 早期内存（nobits，独立段，绝不清零）==================== */
.section ".bootpkt", "aw", @nobits
.align 4096
pml4:  .skip 4096
pdpt:  .skip 4096
pd:    .skip 4096 * 4              /* 4 个 PD：identity 低 4GB */
.align 16
early_stack:
    .skip 16384
early_stack_top:

/* 消除 "missing .note.GNU-stack section implies executable stack" 链接警告 */
.section .note.GNU-stack, "", @progbits
