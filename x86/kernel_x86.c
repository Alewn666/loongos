/* LoongOS x86_64 · 内核主入口（完整版）
 * ----------------------------------------------------------------------------
 * 启动链：GRUB(multiboot2) → boot_x86.S(长模式) → kernel_x86_main
 *   → 串口/IDT/PIC/PIT → Multiboot2 framebuffer → gfx + 字库
 *   → i8042 键鼠(中断上半部) → input 核心层(Linux 模型) → 桌面主循环
 *
 * 与 ARM 版的关系：完全独立的源码树（/x86），ARM 版一行未动。
 * 共享的只有 gfx.c / 字库头文件（纯 C，通过 -I../本体 复用）。
 */
#include <stdint.h>
#include "platform.h"
#include "input.h"
#include "gfx_draw.h"

typedef uint64_t u64;
typedef uint32_t u32;

/* desktop_x86.c */
void desktop_run(void);
void desktop_diag(void);

/* SysV AMD64：rdi = MB2 magic, rsi = MBI 物理地址 */
void kernel_x86_main(u64 magic, u64 mbi)
{
    uart_init();
    uart_puts("\n==================================================\n");
    uart_puts("  LoongOS x86_64  |  纯自研内核 · Linux 输入模型\n");
    uart_puts("==================================================\n");
    uart_puts("[boot] long mode OK, MB2 magic="); uart_hex(magic);
    uart_puts(" mbi="); uart_hex(mbi); uart_puts("\n");

    if (magic != 0x36d76289ULL) {
        uart_puts("[boot] FATAL: not multiboot2, halt\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    extern u64 g_mbi_addr;
    g_mbi_addr = mbi;

    /* ---- 帧缓冲（Multiboot2 tag 8）---- */
    struct fb_info fb = ramfb_setup();
    if (!fb.ok) {
        uart_puts("[fb  ] FATAL: no 32bpp framebuffer tag, halt\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    uart_puts("[fb  ] "); uart_dec((u32)fb.w); uart_puts("x"); uart_dec((u32)fb.h);
    uart_puts(" stride="); uart_dec((u32)fb.stride);
    uart_puts(" base="); uart_hex((u64)(uintptr_t)fb.base); uart_puts("\n");

    /* ---- 绘图 + 输入核心 ---- */
    gfx_init(fb.base, fb.w, fb.h, fb.stride);
    input_init(fb.w, fb.h, 32767, 32767);   /* abs 量程预留（tablet 场景） */
    input_set_repeat(25, 4);                /* 100Hz: 250ms delay / 40ms rate */

    /* ---- 中断体系 ---- */
    idt_init();
    pic_init();
    ps2_init();
    pit_init(100);
    uart_puts("[irq ] IDT/PIC/PIT/i8042 就绪\n");

    irq_enable();
    uart_puts("[desktop] 进入图形桌面；Ctrl+Shift+F1 返回终端\n");

    /* ---- 桌面主循环 ---- */
    desktop_run();

    /* ---- 退出后：串口诊断 + 停机 ---- */
    irq_disable();
    desktop_diag();
    uart_puts("[boot] 桌面已退出，停机。\n");
    for (;;) __asm__ volatile("cli; hlt");
}
