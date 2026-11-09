/* LoongOS x86_64 · 平台层实现
 * 串口(16550) / IDT / 8259 PIC / PIT(100Hz) / Multiboot2 framebuffer 解析。
 * fw_cfg ramfb 不用于 x86：QEMU pc 机默认 fw_cfg 无 DMA，而 ramfb 强制要求
 * DMA；改用标准 Multiboot2 framebuffer tag（VBE / EFI GOP 真机通用）。
 */
#include "platform.h"
#include "input.h"

/* ---------------- 16550 串口 ---------------- */
#define UART 0x3F8
void uart_init(void)
{
    outb(UART+1, 0x00); outb(UART+3, 0x80);
    outb(UART+0, 0x01); outb(UART+1, 0x00);
    outb(UART+3, 0x03); outb(UART+2, 0xC7); outb(UART+4, 0x0B);
}
void uart_putc(char c) { while (!(inb(UART+5)&0x20)){} outb(UART+0,(u8)c); }
void uart_puts(const char *s) { while(*s){ if(*s=='\n')uart_putc('\r'); uart_putc(*s++);} }
void uart_hex(u64 v)
{
    const char *h="0123456789ABCDEF"; uart_puts("0x");
    for(int i=60;i>=0;i-=4) uart_putc(h[(v>>i)&0xF]);
}
void uart_dec(u32 v)
{
    char b[12]; int n=0;
    if(!v){uart_putc('0');return;}
    if((int)v<0){uart_putc('-'); v=(u32)(-(int)v);}
    while(v){b[n++]='0'+v%10;v/=10;}
    while(n) uart_putc(b[--n]);
}

/* ---------------- 系统节拍 ---------------- */
volatile u64 g_ticks;

/* ---------------- IDT ---------------- */
struct idt_entry {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  attr;        /* 0x8E = present, DPL0, 64-bit interrupt gate */
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed));
struct idt_ptr { u16 limit; u64 base; } __attribute__((packed));

static struct idt_entry G_idt[256];
extern u64 isr_stub_0x20_addr, isr_stub_0x21_addr, isr_stub_0x2C_addr, isr_stub_other_addr;

static void idt_set(int v, u64 handler)
{
    G_idt[v].off_lo = (u16)(handler & 0xFFFF);
    G_idt[v].sel    = 0x08;
    G_idt[v].ist    = 0;
    G_idt[v].attr   = 0x8E;
    G_idt[v].off_mid= (u16)((handler >> 16) & 0xFFFF);
    G_idt[v].off_hi = (u32)((handler >> 32) & 0xFFFFFFFF);
    G_idt[v].zero   = 0;
}
void idt_init(void)
{
    u64 other = isr_stub_other_addr;
    for (int v = 0; v < 256; v++) idt_set(v, other);
    idt_set(0x20, isr_stub_0x20_addr);   /* PIT */
    idt_set(0x21, isr_stub_0x21_addr);   /* 键盘 */
    idt_set(0x2C, isr_stub_0x2C_addr);   /* 鼠标 */
    struct idt_ptr p = { (u16)(sizeof(G_idt) - 1), (u64)&G_idt[0] };
    __asm__ volatile("lidt %0" :: "m"(p));
}

/* ---------------- IRQ 分派表 ---------------- */
static irq_handler_t G_irq[16];
void irq_register(int irq, irq_handler_t h) { if (irq>=0 && irq<16) G_irq[irq]=h; }

/* 汇编 irq_common 调用本函数。vec = 中断向量号。 */
void platform_irq_dispatch(u64 vec)
{
    /* 异常（<0x20）与未注册向量：打一次日志后停机，便于定位三重错误 */
    if (vec < 0x20 || vec == 0xFF) {
        uart_puts("\n[irq ] unexpected vector "); uart_hex(vec); uart_puts(" -> halt\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    int irq = (int)(vec - 0x20);
    if (irq == 0) {
        g_ticks++;
        input_tick();                      /* typematic 只由 PIT 节拍驱动 */
    }
    if (irq >= 0 && irq < 16 && G_irq[irq]) G_irq[irq]();

    /* EOI：IRQ>=8 需要先给从片，再给主片 */
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

/* ---------------- 8259 PIC ----------------
 * 重映射 IRQ0..7 → INT 0x20..0x27，IRQ8..15 → 0x28..0x2F。
 * 只放开 IRQ0(PIT)/IRQ1(键盘)/IRQ2(级联)/IRQ12(鼠标)。 */
void pic_init(void)
{
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xF8);            /* 主片：开 IRQ0/1/2，其余屏蔽 (1111 1000) */
    outb(0xA1, 0xEF);            /* 从片：开 IRQ12 (1110 1111) */
}

/* ---------------- PIT ---------------- */
void pit_init(int hz)
{
    if (hz < 1) hz = 100;
    u32 div = 1193182u / (u32)hz;
    if (!div) div = 1;
    outb(0x43, 0x36);            /* ch0, lo/hi, mode 3, binary */
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)(div >> 8));
}

void irq_enable(void)  { __asm__ volatile("sti"); }
void irq_disable(void) { __asm__ volatile("cli"); }

/* ---------------- Multiboot2 framebuffer ----------------
 * 从 MBI 找 tag type=8，返回线性帧缓冲参数。stride 用像素数（pitch/4）。
 * 相比 ARM 版的 fw_cfg ramfb，这条路径依赖 GRUB 的 multiboot2 图形支持，
 * 在 SeaBIOS(VBE) 与 OVMF(GOP) 下都能用，也是真机 NAS 虚拟机的通用方式。 */
struct fb_info ramfb_setup(void)
{
    struct fb_info f;
    f.base = 0; f.w = f.h = f.stride = 0; f.ok = 0;

    /* MBI 地址由 kernel_main 传入并存到全局，见 mbi_probe() */
    extern u64 g_mbi_addr;
    if (!g_mbi_addr) return f;
    u32 total = *(u32 *)g_mbi_addr;
    u8 *p = (u8 *)(g_mbi_addr + 8);
    u8 *end = (u8 *)(g_mbi_addr + total);
    while (p + 8 <= end) {
        struct mb2_tag { u16 type; u16 flags; u32 size; } __attribute__((packed));
        struct mb2_tag *t = (struct mb2_tag *)p;
        if (t->type == 0 && t->size == 8) break;
        if (t->type == 8 && t->size >= 30) {
            u64 addr   = *(u64 *)(p + 8);
            u32 pitch  = *(u32 *)(p + 16);
            u32 width  = *(u32 *)(p + 20);
            u32 height = *(u32 *)(p + 24);
            u8  bpp    = *(u8  *)(p + 28);
            if (addr && bpp == 32 && width && height) {
                f.base = (u32 *)addr;
                f.w = (int)width; f.h = (int)height;
                f.stride = (int)(pitch / 4);
                f.ok = 1;
                return f;
            }
        }
        u32 sz = t->size < 8 ? 8 : (t->size + 7) & ~7u;
        p += sz;
    }
    return f;
}

u64 g_mbi_addr;   /* kernel_main 保存的 MBI 物理地址 */
