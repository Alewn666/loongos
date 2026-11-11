/* LoongOS 链接脚本 v2（阶段2；原名 MyOS）：
 * - PHDRS 把代码/只读数据/可写数据拆成三个 LOAD 段（R+X / R / RW），
 *   消掉 stage0 的 "LOAD segment with RWX permissions" 警告，
 *   也给 MMU 分权限映射提供节区边界符号；
 * - 新增 .el0text：EL0 用户态演示代码独占页（页表里单独标"EL0 可执行"）。 */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* 4(R)+1(X) = R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* 2(W)+4(R) = RW */
}

SECTIONS
{
    . = 0x40080000;
    __kernel_start = .;

    .text : {
        __text_start = .;
        *(.text.boot)          /* 启动汇编必须放最前面 */
        *(.text .text.*)
        __text_end = .;
    } :text

    .el0text : ALIGN(0x1000) { /* 4KB 对齐：独占页表页，权限与内核 text 分开 */
        __el0text_start = .;
        *(.el0text .el0text.*)
        . = ALIGN(0x1000);
        __el0text_end = .;
    } :text

    .rodata : ALIGN(16) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :rodata

    .data : ALIGN(16) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        /* 阶段三扩容方案1（治本）：所有 4KB 对齐的设备缓冲（页表 / virtio 队列 /
         * NVMe 之外的 DMA 页）统一收进 .bss.align4k，置于普通小变量之后、内核栈之前。
         * 这样小变量再怎么重排都不会把整条对齐链顶后一页，±4KB 敏感度消除，
         * 上界余量可预期（见 docs/内核空间预算与扩容指引.md 第六节）。 */
        . = ALIGN(4096);
        *(.bss.align4k)
        . = ALIGN(16);
        _stack_bottom = .;
        . += 32 * 1024;        /* 32KB 内核栈（64KB→40KB→32KB；阶段6③ 加了 virtio 队列 8KB+4KB 缓冲，给 bss 让出余量，
                                  USER_BASE 前的空间必须留白，否则新变量会压到用户区） */
        _stack_top = .;
        __bss_end = .;
    } :data

    /* NVMe DMA 区（2026-11-08 新增）
     * ------------------------------------------------------------------
     * 为什么必须单独一段、且放在内核栈之后：
     *   1) NVMe 要求 ASQ/ACQ/IO SQ/IO CQ/DMA 缓冲全部 4KB 页对齐
     *      （QEMU nvme_start_ctrl() 对 ASQ/ACQ 做 (page_size-1) 掩码检查）。
     *   2) 本段整体 4KB 对齐，内部每页 4KB 对齐，天然满足要求。
     *   3) **必须排在 .bss 之后**：.bss 里的成员按声明顺序排列，而内核栈
     *      (_stack_bottom.._stack_top) 是在 .bss 成员**之后**才分配的。
     *      若把 20KB 的 DMA 缓冲放进 .bss，链接器会把栈顶推到缓冲中间，
     *      导致内核栈覆盖 DMA 缓冲（实测 nv_mem=0x401f4000 而
     *      _stack_bottom=0x401f7010 → 直接踩坏队列）。
     *      放在栈之后，两者彻底分开。
     *   4) __bss_end 仍是内核 .bss 的真实上界（构建期 BSS 守卫读它），
     *      本段在其之后，需要同时更新守卫的边界检查。
     */
    .nvmedma : ALIGN(4096) {
        __nvmedma_start = .;
        KEEP(*(.nvmedma .nvmedma.*))
        . = ALIGN(4096);
        __nvmedma_end = .;
    } :data

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
