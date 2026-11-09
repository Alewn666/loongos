/* LoongOS x86_64 · i8042 PS/2 键鼠驱动
 * ----------------------------------------------------------------------------
 * 这是「设备层」，只做两件事：
 *   1) 从 0x60/0x64 读硬件原始字节；
 *   2) 翻译成标准 input_event 上报给输入核心层（input.c）。
 * 绝不直接改任何全局坐标/按钮变量 —— 这正是旧 ARM 版「一坨」的根因。
 *
 * 对照 Linux：
 *   ps2_kbd_irq   ~ drivers/input/keyboard/atkbd.c 的 atkbd_interrupt()
 *   ps2_mouse_irq ~ drivers/input/mouse/psmouse-base.c 的 psmouse_interrupt()
 *   两者都只 input_report()/input_sync()，状态归约交给 input 核心层。
 */
#include "platform.h"
#include "input.h"

/* ---- i8042 端口 ---- */
#define KBD_STATUS 0x64
#define KBD_DATA   0x60
#define KBD_CMD    0x64

static inline int kbd_can_read(void) { return inb(KBD_STATUS) & 1; }
static void kbd_wait_write(void)    { while (inb(KBD_STATUS) & 2){} }

static void aux_write(u8 b)          /* 给鼠标发命令 */
{
    kbd_wait_write(); outb(KBD_CMD, 0xD4);
    kbd_wait_write(); outb(KBD_DATA, b);
    /* 等 ACK（0xFA）；这里简化为等一个字节 */
    for (int i = 0; i < 100000 && !kbd_can_read(); i++) {}
    if (kbd_can_read()) (void)inb(KBD_DATA);
}

/* ---- 键盘（IRQ1）----
 * set1 scancode：make = 0x00..0x7F，break = make | 0x80。
 * 0xE0 前缀（扩展键）这里做一个最小状态机，把方向键/小键盘等映射为
 * 「忽略」（本轮桌面只用主键盘区），避免把 0xE0 当成一个键报出去。 */
static int G_e0;
void ps2_kbd_irq(void)
{
    if (!kbd_can_read()) return;
    u8 raw = inb(KBD_DATA);
    if (raw == 0xE0) { G_e0 = 1; return; }
    if (raw == 0xE1) { G_e0 = 2; return; }   /* Pause 键序列，忽略后续 */

    if (G_e0) {
        /* 扩展键：目前忽略，但保留位图语义（不影响主键盘区修饰键） */
        G_e0 = 0;
        return;
    }

    int down = !(raw & 0x80);
    u16 code = (u16)(raw & 0x7F);
    input_report(EV_KEY, code, down ? KEY_VAL_DOWN : KEY_VAL_UP);
    input_sync();                              /* 一个按键 = 一个原子帧 */
}

/* ---- 鼠标（IRQ12）----
 * PS/2 标准 3 字节包：
 *   b0: bit0=左 bit1=右 bit2=中 bit3=1(always) bit4=X符号 bit5=Y符号
 *       bit6=X溢出 bit7=Y溢出
 *   b1: X 位移(9 位补码的低 8 位)
 *   b2: Y 位移
 * 一个数据包 = 一个原子帧：位移 + 按钮边沿一起入队，input_sync 封帧。
 * 按钮用 input_report_btn_delta 只发「变化位」，从根上杜绝多设备互相覆盖。 */
static int G_mphase;
static u8  G_mb[3];
static int G_prev_btn;

void ps2_mouse_irq(void)
{
    if (!kbd_can_read()) return;
    u8 v = inb(KBD_DATA);
    /* 用 bit3=1 作为包起始同步，防止中途错位 */
    if (G_mphase == 0 && !(v & 0x08)) return;
    G_mb[G_mphase++] = v;
    if (G_mphase < 3) return;
    G_mphase = 0;

    u8 b0 = G_mb[0], bx = G_mb[1], by = G_mb[2];
    if (b0 & 0x40 || b0 & 0x80) return;       /* 溢出包丢弃 */

    /* 9 位补码符号扩展（Linux psmouse 惯用写法）：
       X 符号位在 b0.bit4，Y 符号位在 b0.bit5，分别是第 9 位。 */
    int dx = (int)bx - (int)((b0 << 4) & 0x100);
    int dy = (int)by - (int)((b0 << 3) & 0x100);

    int cur_btn = b0 & 7;

    /* 先报位移，再报按钮边沿，最后一个 sync 封成一帧 */
    if (dx) input_report(EV_REL, REL_X, dx);
    if (dy) input_report(EV_REL, REL_Y, -dy);  /* 屏幕 Y 向下，PS/2 Y 向上 → 取反 */
    input_report_btn_delta(G_prev_btn, cur_btn);
    G_prev_btn = cur_btn;
    input_sync();
}

/* ---- 初始化 ---- */
int ps2_init(void)
{
    /* 关闭键盘/鼠标中断（配置期间），开中断由调用方最后统一 sti */
    kbd_wait_write(); outb(KBD_CMD, 0xAD);      /* disable keyboard */
    kbd_wait_write(); outb(KBD_CMD, 0xA7);      /* disable mouse */

    /* 清空可能残留的输出缓冲 */
    for (int i = 0; i < 32 && kbd_can_read(); i++) (void)inb(KBD_DATA);

    /* 读配置字节，打开 IRQ1 + IRQ12，并确保鼠标时钟未禁用 */
    kbd_wait_write(); outb(KBD_CMD, 0x20);
    for (int i = 0; i < 100000 && !kbd_can_read(); i++) {}
    u8 cfg = kbd_can_read() ? inb(KBD_DATA) : 0;
    cfg |= 0x03;                                /* IRQ1 + IRQ12 enable */
    cfg &= ~(u8)0x20;                           /* 清 mouse disable(bit5) */
    cfg &= ~(u8)0x10;                           /* 清 keyboard disable(bit4) */
    cfg |= 0x40;                                /* 打开 scancode translation(set1) */
    kbd_wait_write(); outb(KBD_CMD, 0x60);
    kbd_wait_write(); outb(KBD_DATA, cfg);

    kbd_wait_write(); outb(KBD_CMD, 0xA8);      /* enable mouse */
    aux_write(0xF6);                            /* set defaults */
    aux_write(0xF4);                            /* enable data reporting */

    kbd_wait_write(); outb(KBD_CMD, 0xAE);      /* enable keyboard */

    /* 注册 IRQ */
    irq_register(1,  ps2_kbd_irq);
    irq_register(12, ps2_mouse_irq);

    G_mphase = 0; G_prev_btn = 0; G_e0 = 0;
    return 0;
}
