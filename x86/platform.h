/* LoongOS x86_64 · 平台层公共接口
 * 全部 port I/O 内联；架构相关的初始化集中在此，C 逻辑层通过本头文件访问。
 */
#ifndef LOONGOS_PLATFORM_H
#define LOONGOS_PLATFORM_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- port I/O ---- */
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8   inb (u16 p)      { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u16 p, u16 v){ __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void io_wait(void)     { outb(0x80, 0); }   /* 传统 ISA 延时口 */

/* ---- 16550 串口（COM1） ---- */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_hex(u64 v);
void uart_dec(u32 v);

/* ---- IDT / PIC / PIT ---- */
void idt_init(void);
void pic_init(void);
void pit_init(int hz);
void irq_enable(void);                 /* sti */
void irq_disable(void);                /* cli */
/* 汇编 ISR common 调用：按中断向量分派 + 发 EOI（在 platform.c 实现） */
void platform_irq_dispatch(u64 vec);
/* 注册某个 IRQ(0..15) 的 C 处理函数 */
typedef void (*irq_handler_t)(void);
void irq_register(int irq, irq_handler_t h);

/* ---- 系统节拍 ---- */
extern volatile u64 g_ticks;

/* ---- fw_cfg / ramfb 帧缓冲 ---- */
struct fb_info {
    u32  *base;
    int   w, h, stride;      /* stride 为像素数 */
    int   ok;
};
/* 通过 fw_cfg 的 etc/ramfb 配置线性帧缓冲，返回填充好的 fb_info。
   失败时 ok=0。FB 物理地址取链接脚本的 __fb_start。 */
struct fb_info ramfb_setup(void);

/* 帧缓冲物理基址（链接脚本符号） */
extern u8 __fb_start[];

/* ---- i8042 PS/2 键鼠 ---- */
int  ps2_init(void);                   /* 0 成功 */
void ps2_kbd_irq(void);                /* IRQ1 上半部 */
void ps2_mouse_irq(void);              /* IRQ12 上半部 */

#endif /* LOONGOS_PLATFORM_H */
