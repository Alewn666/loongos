/* LoongOS x86_64 链接脚本
 * ----------------------------------------------------------------------------
 * 物理地址 1MB 起（0x100000）：QEMU -kernel 与 GRUB multiboot2 都把内核加载到
 * 1MB 以上的传统位置。identity 映射低 1GB（见 boot_x86.S），故链接地址 = 物理地址。
 *
 * 内存布局（互不重叠，全部落在 identity 映射的低 1GB 内）：
 *   0x00100000  内核 .text/.rodata/.data/.bss
 *   0x01000000  .bootpkt：页表(3×4K) + GDT + 16K 早期栈（nobits，启动汇编显式初始化）
 *   0x02000000  帧缓冲：ramfb 把这块物理内存登记给 QEMU 显示，800×600×4 ≈ 1.9MB
 *
 * PHDRS 拆三个 LOAD 段（R+X / R / RW），消除 "LOAD segment with RWX" 警告，
 * 也为日后 MMU 分权限映射留出节区边界。
 */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* RW  */
}

SECTIONS
{
    . = 0x100000;
    __kernel_start = .;

    .text : {
        KEEP(*(.multiboot))        /* MB2 header 必须在镜像最前 32KB 内 */
        *(.text.boot)
        *(.text .text.*)
    } :text

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
        *(.rodata.gdt)
    } :rodata

    .data : ALIGN(16) {
        *(.data .data.*)
    } :data

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /* 早期内存（页表/GDT/栈）：nobits，独立于 .bss，绝不被清零 */
    . = 0x1000000;
    .bootpkt : ALIGN(4096) {
        *(.bootpkt .bootpkt.*)
    } :data

    /* 帧缓冲物理基址：只是一个符号，不与任何 section 争空间。
       ramfb 配置里把 FB_PHYS 填这个值。 800×600×4 = 1,920,000 B < 16MB 空隙。 */
    . = 0x2000000;
    __fb_start = .;
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.note.gnu.build-id) }
}
