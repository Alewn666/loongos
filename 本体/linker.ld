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
        . = ALIGN(16);
        _stack_bottom = .;
        . += 32 * 1024;        /* 32KB 内核栈（64KB→40KB→32KB；阶段6③ 加了 virtio 队列 8KB+4KB 缓冲，给 bss 让出余量，
                                  USER_BASE 前的空间必须留白，否则新变量会压到用户区） */
        _stack_top = .;
        __bss_end = .;
    } :data

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
