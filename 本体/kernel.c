/* LoongOS 阶段5④ - 重定向（cmd > 输出到文件、write < 输入喂文件）
 * 阶段5① 起能力全保留：1MB 内存盘 fs（Unix 单根 /）、行输入 shell、root/demo + 口令 + DAC。
 * 本阶段新增：
 *   - 输出重定向：uart_putc 是唯一总闸，redir_on 期间命令的打印先进 2KB 缓冲不上串口，
 *     命令跑完 redir_finish() 一次性落盘（覆盖写）并回报字节数；worker 心跳打印在重定向期间
 *     闭嘴（防抢占时把心跳写进文件）；未知命令的抱怨收闸后才发串口 = 相当于 stderr 不进文件
 *   - 输入重定向：write 目标 < 源 —— 源文件整份读进 stdin_buf（≤2047 字节），目前只有 write
 *     消费（= 文件复制）；其他命令吃 < 会明确报"不支持"
 *   - 语法：redir_parse 拆词，> 和 < 必须独立成词、各只一次、后面紧跟路径，否则报语法错
 * 口令只存哈希：auth_cred.h 里是预计算的 5 万轮迭代 SHA-256（内核镜像里没有明文口令）。
 * 默认口令（教学演示，只写在注释和 docs 里、不进镜像）：root=loong2026 demo=demo2026。 */

#include "fs.h"     /* fs.c 的接口（typedef 与本文件重复但同型，C11 允许） */
#include "crypto.h" /* SHA-256 + 迭代哈希（口令校验） */
#include "auth_cred.h" /* 默认账号凭据：盐/迭代数/预计算哈希（内核镜像零明文口令） */
#include "usb_kbd.h"/* 阶段6①：xHCI + USB HID 键盘 */

typedef unsigned long u64;
typedef unsigned int  u32;
typedef unsigned char u8;

/* ---------------- PL011 串口（QEMU virt @ 0x09000000, IRQ 33） ---------------- */
#define UART0_BASE 0x09000000UL
#define UART_DR    (*(volatile u32 *)(UART0_BASE + 0x00))
#define UART_FR    (*(volatile u32 *)(UART0_BASE + 0x18))
#define UART_IMSC  (*(volatile u32 *)(UART0_BASE + 0x38))
#define UART_IBRD  (*(volatile u32 *)(UART0_BASE + 0x24))
#define UART_FBRD  (*(volatile u32 *)(UART0_BASE + 0x28))
#define UART_LCRH  (*(volatile u32 *)(UART0_BASE + 0x2C))
#define UART_CR    (*(volatile u32 *)(UART0_BASE + 0x30))

/* ---- 阶段6③：virtio-blk（virtio_blk.c） ---- */
extern int  vb_init(void);
/* ---- virtio-scsi（virtio_scsi.c）：虚拟机把硬盘挂成 scsi 总线时的驱动 ---- */
extern int  vs_init(void);
extern int  vs_present(void);
extern unsigned long long vs_capacity_bytes(void);
extern int  vb_present(void);
extern int  vb_modern_only(void);
extern u64  vb_capacity_bytes(void);
extern u64  vb_capacity_sectors(void);
extern int  vb_read(u64 lba, void *buf, u32 nsec);
extern int  vb_write(u64 lba, const void *buf, u32 nsec);
extern int  fs_format_disk(u64 part_lba, u32 nblocks);
extern int  fs_mount_disk(u64 part_lba, u32 nblocks);
extern int  fs_is_disk(void);
extern u32  fs_blocks(void);
/* 阶段7：联网（virtio_net.c 驱动 + net.c 协议栈） */
extern int  net_init_driver(void);
extern int  net_dhcp(void);
extern int  net_static_config(const char *ip, const char *mask, const char *gw);
extern void net_ifconfig_print(void);
extern int  net_ping(const char *ip, int count);
extern void net_poll(void);
/* 阶段9：SMP */
extern int  smp_start(unsigned long *cpu_mask);
extern void smp_set_gic_ver(int v);
extern int  smp_wait_online(unsigned long cpu, int spins);
extern unsigned long smp_cpu_id(void);
extern volatile unsigned long smp_online[4];
extern volatile unsigned long smp_counter[4];

/* 阶段8：TCP */
extern int  tcp_connect(unsigned int ip, unsigned short port, int timeout_ms);
extern int  tcp_send(const void *buf, int len);
extern int  tcp_recv(void *buf, int max, int timeout_ms);
extern void tcp_close(void);
extern int  tcp_established(void);
#define FR_TXFF    (1u << 5)
#define FR_RXFE    (1u << 4)
/* ⚠️ QEMU pl011：INT_RX=bit4、INT_TX=bit5（开错位=发一个字就中断风暴）；
   ⚠️ 绝不写 ICR：会抹掉早期字符的 RX 挂起位 → 接收死锁（阶段1 踩过） */
#define IMSC_RXIM  (1u << 4)
#define UART0_IRQ  33

/* ---------------- GICv2（QEMU virt, gic-version=2） ---------------- */
#define GICD_BASE  0x08000000UL
#define GICC_BASE  0x08010000UL
#define GICD_CTLR   (*(volatile u32 *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile u32 *)(GICD_BASE + 0x100 + 4 * ((n) / 32)))
#define GICD_IPRIORITYR(n) (*(volatile u8 *)(GICD_BASE + 0x400 + (n)))
#define GICC_CTLR   (*(volatile u32 *)(GICC_BASE + 0x000))
#define GICC_PMR    (*(volatile u32 *)(GICC_BASE + 0x004))
#define GICC_IAR    (*(volatile u32 *)(GICC_BASE + 0x00C))
#define GICC_EOIR   (*(volatile u32 *)(GICC_BASE + 0x010))

/* ---- GICv3（KVM 常见）：CPU 接口改成系统寄存器，GICv2 的 0x08010000 区域不存在 ----
 * 踩坑：写死 GICv2 时，在 GICv3 的虚拟机上第一次访问 GICC_PMR 就吃数据中止
 *   FAR_EL1=0x08010004  ESR EC=0x25（DABT from current EL）→ 内核当场死。
 * 判版本：GICD_PIDR2(0xFE8) 的 [7:4] = 架构版本（0x2=GICv2, 0x3=GICv3） */
/* ⚠️ ID 寄存器位置随版本不同：
 *   GICv2 → 0xFE8（4KB 帧内）
 *   GICv3 → 0xFFE8（64KB 帧的最后一页；0xFE8 处读出来是 0）
 * 只看 0xFE8 会把 GICv3 误判成 GICv2，然后写 0x08010000 吃数据中止（踩过）。 */
#define GICD_PIDR2_V2   (*(volatile u32 *)(GICD_BASE + 0xFE8))
#define GICD_PIDR2_V3   (*(volatile u32 *)(GICD_BASE + 0xFFE8))
#define GICD_IGROUPR(n) (*(volatile u32 *)(GICD_BASE + 0x080 + 4 * ((n) / 32)))
#define GICR_BASE      0x080A0000UL          /* QEMU virt 的 redistributor 基址 */
#define GICR_SGI_BASE  (GICR_BASE + 0x10000UL)
#define GICR_CTLR      (*(volatile u32 *)(GICR_BASE + 0x000))
#define GICR_IGROUPR0  (*(volatile u32 *)(GICR_SGI_BASE + 0x080))
#define GICR_ISENABLER0 (*(volatile u32 *)(GICR_SGI_BASE + 0x100))
#define GICR_IPRIORITYR(n) (*(volatile u8 *)(GICR_SGI_BASE + 0x400 + (n)))

static u32 gic_ver = 2;      /* 2 或 3，gic_init 里探测 */

static inline u64 icc_iar1_read(void)  { u64 v; __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(v)); return v; }
static inline void icc_eoir1_write(u64 v) { __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(v)); }

/* ---------------- 异常帧（与 boot.S 的 SAVE_CTX 布局一致） ---------------- */
struct regs {
    u64 x[31];      /* x0..x30 */
    u64 elr, spsr, esr, far;
};

/* ---------------- 阶段5④：重定向状态（uart_putc 是唯一总闸，shell 命令的打印都从这过） ---------------- */
static int  redir_on   = 0;          /* 输出重定向进行中：uart 输出进缓冲不上串口 */
static int  redir_len  = -1;         /* 缓冲内字节数；-1=本次没有输出重定向 */
static int  redir_of   = 0;          /* 超 2KB 被截断过 */
static char redir_path[128];         /* 输出目标（已规范化绝对路径） */
static char redir_buf[2048];
static char stdin_buf[2048];         /* 输入重定向：< 文件内容整份读进来 */
static int  redir_in_len = -1;       /* < 文件字节数；-1=没有输入重定向（只有 write 能吃） */

/* ================= 帧缓冲（GOP）文字输出 =================
 * 为什么需要：用户真机实测——虚拟机窗口显示的是 UEFI 图形控制台(GOP)，
 * 而内核写的是串口，两边不通，屏幕上什么都看不到（内核其实活着）。
 * stub 通过 bootinfo 把 GOP 帧缓冲参数传进来，这里直接往帧缓冲画 8x16 点阵。 */
#include "font8x16.h"
#include "font_cjk.h"

struct bootinfo {
    u32 magic, width, height, stride, pixfmt, bpp;
    u64 base, size;
    /* 阶段6③：stub 从 ISO 里读出来的 ESP 镜像（4MB FAT，含 BOOTAA64.EFI + kernel.elf），
       安装器把它按扇区原样写进硬盘的 ESP 分区 —— 这样内核里完全不需要 FAT 写入代码。 */
    u64 img_addr, img_size;
};
#define BOOTINFO_MAGIC 0x4C4F4F53u

static struct bootinfo *g_bi;
static volatile u32 *fb_mem;
static u32 fb_w, fb_h, fb_stride;
static u32 fb_x, fb_y;
/* 屏幕文字边距：文字不要贴边，否则被网页控制台的边框盖住。
   2026-10-25 第二轮：16px 仍被飞牛网页控制台（等比缩放+裁剪）切掉左侧约 14px，
   实测网页端把 800x600 放大到 852 宽显示，左右各裁掉十几像素 → 第一列字被切。
   改为 48/24（≈6 字符宽），留足裁剪余量。 */
#define FB_MX 48
#define FB_MY 24

/* 帧缓冲能否用 8 字节批量写：基址 8 字节对齐 + 行跨度是偶数（每像素 4 字节） */
static int fb_u64_ok;


/* 16 字节粒度拷贝（ldp/stp）：帧缓冲现在是 Normal-WB，走缓存，
   8 字节一对的内存操作在这里是瓶颈。n8 = 8 字节单元个数。 */
static void fb_copy16(u64 *d, const u64 *s, u32 n8)
{
    u32 i = 0;
    for (; i + 2 <= n8; i += 2) {
        u64 a, b;
        __asm__ volatile("ldp %0, %1, [%2]" : "=r"(a), "=r"(b) : "r"(s + i));
        __asm__ volatile("stp %0, %1, [%2]" :: "r"(a), "r"(b), "r"(d + i) : "memory");
    }
    for (; i < n8; i++) d[i] = s[i];
}

/* 16 字节粒度清零 */
static void fb_zero16(u64 *d, u32 n8)
{
    u32 i = 0;
    for (; i + 2 <= n8; i += 2)
        __asm__ volatile("stp xzr, xzr, [%0]" :: "r"(d + i) : "memory");
    for (; i < n8; i++) d[i] = 0;
}

static void fb_clear_all(void)
{
    if (!fb_mem) return;
    if (fb_u64_ok) {
        for (u32 y = 0; y < fb_h; y++) {
            u64 *d = (u64 *)(fb_mem + (u64)y * fb_stride);
            fb_zero16(d, fb_w / 2);
            if (fb_w & 1) fb_mem[(u64)y * fb_stride + fb_w - 1] = 0;
        }
    } else {
        for (u32 y = 0; y < fb_h; y++)
            for (u32 x = 0; x < fb_w; x++)
                fb_mem[(u64)y * fb_stride + x] = 0;
    }
}

static void fb_scroll(void)
{
    u32 rows = fb_h - FONT_H;
    if (fb_u64_ok) {
        for (u32 y = FB_MY; y < rows; y++) {             /* 文字区上移一行（顶部边距不动） */
            u64 *d = (u64 *)(fb_mem + (u64)y * fb_stride);
            const u64 *s = (const u64 *)(fb_mem + (u64)(y + FONT_H) * fb_stride);
            fb_copy16(d, s, fb_w / 2);
            if (fb_w & 1) fb_mem[(u64)y * fb_stride + fb_w - 1] =
                              fb_mem[(u64)(y + FONT_H) * fb_stride + fb_w - 1];
        }
        for (u32 y = rows; y < fb_h; y++) {               /* 底部一行清黑 */
            u64 *d = (u64 *)(fb_mem + (u64)y * fb_stride);
            fb_zero16(d, fb_w / 2);
            if (fb_w & 1) fb_mem[(u64)y * fb_stride + fb_w - 1] = 0;
        }
    } else {
        for (u32 y = FB_MY; y + FONT_H < fb_h; y++)
            for (u32 x = 0; x < fb_w; x++)
                fb_mem[(u64)y * fb_stride + x] = fb_mem[(u64)(y + FONT_H) * fb_stride + x];
        for (u32 y = rows; y < fb_h; y++)
            for (u32 x = 0; x < fb_w; x++)
                fb_mem[(u64)y * fb_stride + x] = 0;
    }
}

/* 汉字查找（font_cjk 已按码点排序，二分） */
static const cjk_glyph *cjk_find(u32 cp)
{
    int lo = 0, hi = CJK_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_cjk[mid].cp == cp) return &font_cjk[mid];
        if (font_cjk[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* 换行。⚠️ 必须当场把 fb_y 收回有效区：
   2026-10-29 真机抓到「屏幕满后每条命令输出第一个字被吞」——根因就是这里原来
   只 `fb_y += FONT_H` 不检查，fb_y 被推到屏幕底外(=fb_h)后，下一个字符的绘制循环
   `if (y >= fb_h) break;` 会整字丢弃，而滚动检查在字符画完之后才跑（晚了）。
   串口终端看不出来（它是另一条路），用户 VM 没串口 → 每条输出肉眼少第一个字
   （cat 出 "ello…"、"已经是" 变 "经是"、pwd 的 "/" 整个不见）。
   改成换行即滚：屏幕满时 '\n' 当场上滚一行，后续字符永远落在有效行上。 */
static void fb_newline(void)
{
    fb_x = FB_MX;
    fb_y += FONT_H;
    while (fb_y + FONT_H > fb_h) {
        if (fb_y < FONT_H) { fb_y = FB_MY; break; }   /* 荒谬小屏兜底，防下溢死循环 */
        fb_scroll();
        fb_y -= FONT_H;
    }
}

/* 把一个码点画到屏幕 */
static void fb_draw_cp(u32 cp)
{
    if (!fb_mem) return;
    if (cp == '\n') { fb_newline(); return; }
    if (cp == '\r') { fb_x = FB_MX; return; }
    if (cp == '\t') { fb_x = FB_MX + ((fb_x - FB_MX) / (FONT_W * 4) + 1) * FONT_W * 4; return; }
    /* 退格（0x08）：光标左移一格并把该格抹黑。
       ⚠️ 必须在这里处理，否则会掉进下面的「画字符」分支被画成 '?' ——
       shell 的退格是 `uart_puts("\b \b")`，串口终端本来就能吃，屏幕上却看不出删除
       （2026-10-24 用户反馈「删除键没做」的真因）。
       行首退格忽略（shell 不会跨行退，行内左侧就是提示符）。 */
    if (cp == '\b') {
        if (fb_x >= FB_MX + FONT_W) {
            fb_x -= FONT_W;
            for (u32 r = 0; r < FONT_H && fb_y + r < fb_h; r++) {
                if (fb_u64_ok) {
                    u64 *d = (u64 *)(fb_mem + (u64)(fb_y + r) * fb_stride + fb_x);
                    fb_zero16(d, FONT_W / 2);
                } else {
                    for (u32 cx = 0; cx < FONT_W; cx++)
                        fb_mem[(u64)(fb_y + r) * fb_stride + fb_x + cx] = 0;
                }
            }
        }
        return;
    }

    if (cp < 128) {
        unsigned char ch = (unsigned char)cp;
        if (ch < FONT_FIRST || ch >= FONT_FIRST + FONT_COUNT) ch = '?';
        const unsigned char *g = font8x16[ch - FONT_FIRST];
        if (fb_x + FONT_W > fb_w - FB_MX) fb_newline();
        for (u32 r = 0; r < FONT_H; r++) {
            u32 y = fb_y + r;
            if (y >= fb_h) break;
            unsigned char bits = g[r];
            /* ⚠️ 不要改成 u64 批量写：实测字形会失真（屏幕上出现重影）。
               字形是 8 像素/行，u64 = 2 像素，跨行边界时对齐假设不成立。
               帧缓冲已经是 Normal-NC，逐像素写本身就够快（NC 前 3679us → NC 后 1410us）。 */
            for (u32 cx = 0; cx < FONT_W; cx++)
                fb_mem[(u64)y * fb_stride + fb_x + cx] =
                    ((bits >> (7 - cx)) & 1) ? 0x00FFFFFFu : 0x00000000u;
        }
        fb_x += FONT_W;
    } else {
        const cjk_glyph *g = cjk_find(cp);
        if (fb_x + CJK_W > fb_w - FB_MX) fb_newline();
        for (u32 r = 0; r < CJK_H; r++) {
            u32 y = fb_y + r;
            if (y >= fb_h) break;
            unsigned hi = g ? g->bmp[r * 2] : 0, lo = g ? g->bmp[r * 2 + 1] : 0;
            if (!g) { hi = 0x7E; lo = 0x7E; }          /* 缺字画个框 */
            for (u32 cx = 0; cx < 8; cx++)
                fb_mem[(u64)y * fb_stride + fb_x + cx] = (hi >> (7 - cx)) & 1 ? 0x00FFFFFFu : 0;
            for (u32 cx = 0; cx < 8; cx++)
                fb_mem[(u64)y * fb_stride + fb_x + 8 + cx] = (lo >> (7 - cx)) & 1 ? 0x00FFFFFFu : 0;
        }
        fb_x += CJK_W;
    }
    while (fb_y + FONT_H > fb_h) { fb_scroll(); fb_y -= FONT_H; }
}

/* UTF-8 解码：串口一个字节一个字节地喂进来，这里攒成码点 */
static u32 fb_u8cp; static int fb_u8need;
static void fb_putc(char c)
{
    unsigned char b = (unsigned char)c;
    if (!fb_mem) return;
    if (fb_u8need) {
        if ((b & 0xC0) == 0x80) {
            fb_u8cp = (fb_u8cp << 6) | (b & 0x3F);
            if (--fb_u8need == 0) fb_draw_cp(fb_u8cp);
            return;
        }
        fb_u8need = 0;                      /* 坏序列：丢弃，重新判断 */
    }
    if (b < 0x80) { fb_draw_cp(b); return; }
    if ((b & 0xE0) == 0xC0) { fb_u8cp = b & 0x1F; fb_u8need = 1; }
    else if ((b & 0xF0) == 0xE0) { fb_u8cp = b & 0x0F; fb_u8need = 2; }
    else if ((b & 0xF8) == 0xF0) { fb_u8cp = b & 0x07; fb_u8need = 3; }
    else fb_draw_cp('?');
}

static void fb_init(void)
{
    if (!g_bi || g_bi->magic != BOOTINFO_MAGIC) return;
    if (g_bi->bpp != 32 || !g_bi->base || !g_bi->width || !g_bi->height) return;
    fb_mem = (volatile u32 *)(u64)g_bi->base;
    fb_w = g_bi->width;
    fb_h = g_bi->height;
    fb_stride = g_bi->stride ? g_bi->stride : g_bi->width;
    fb_x = FB_MX; fb_y = FB_MY;
    /* 批量写的前提：基址 8 字节对齐、行跨度偶数（每像素 4 字节 → 行首始终 8 字节对齐） */
    fb_u64_ok = (((u64)fb_mem & 7) == 0) && ((fb_stride & 1) == 0);
    /* 清屏：把 UEFI 留下的旧文字抹掉 */
    fb_clear_all();
}

/* ---------------- 串口输出（忙等轮询发，中断里也能安全用） ---------------- */
/* 【实验记录 2026-10-25】曾试着把 PL011 分频改成 921600（IBRD=1,FBRD=40）想加速文本输出，
   实测**无效**：QEMU 的 PL011 不按分频节流（首行 396→396、shell 469 vs 339ms 反而更慢，
   属于噪声）。39us/字节是 emulation 的固有速率 → 已撤回，避免真机终端波特率不匹配。
   若以后要提速，方向是「TX 环形缓冲 + TX 中断」，让屏幕出字不被串口等待阻塞。 */

static void uart_putc(char c)
{
    if (redir_on) {                  /* 阶段5④：重定向期间收进缓冲，不发串口 */
        if (redir_len < (int)sizeof(redir_buf) - 1)
            redir_buf[redir_len++] = c;
        else if (!redir_of)          /* 超限只标一次（文件里会有"已截断"提示） */
            redir_of = 1;
        return;
    }
    /* 阶段9：多核下所有输出走一把锁（Linux console_lock 简化版），
       只护住「写硬件」这一小段，别把重定向判断也圈进来 */
    extern volatile int console_lock;
    extern volatile int smp_locks_on;
    extern void spin_lock(volatile int *);
    extern void spin_unlock(volatile int *);
    if (smp_locks_on) spin_lock(&console_lock);   /* MMU 前是 Device 内存，不能用 ldaxr/stxr */
    while (UART_FR & FR_TXFF) { }
    UART_DR = (u32)c;
    fb_putc(c);                      /* 同一份输出也画到 GOP 帧缓冲 */
    if (smp_locks_on) spin_unlock(&console_lock);
}
static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}
static void uart_puthex(u64 v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nib = (int)((v >> (60 - 4 * i)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = 0;
    uart_puts(buf);
}
static void uart_puthex2(u8 v)
{
    const char *H = "0123456789abcdef";
    uart_putc(H[v >> 4]);
    uart_putc(H[v & 15]);
}
static void uart_putdec(u32 v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) uart_putc(buf[--i]);
}

/* 后台心跳专用输出：只写串口、不画 GOP 帧缓冲。
   真机（ramfb 图形控制台）上屏幕就是唯一可见通道，心跳画上去会一直刷屏、
   把 shell 挤出可视区；串口侧保留（自动化测试/调试靠它判断调度活着）。 */
static void hb_putc(char c)
{
    while (UART_FR & FR_TXFF) { }
    UART_DR = (u32)c;
}
static void hb_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            hb_putc('\r');
        hb_putc(*s++);
    }
}
static void hb_puthex(u64 v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nib = (int)((v >> (60 - 4 * i)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = 0;
    hb_puts(buf);
}

/* ---------------- 接收环形缓冲（中断塞字节，主循环取） ---------------- */
#define RING_SZ 64
static volatile char ring[RING_SZ];
static volatile u32 ring_head = 0, ring_tail = 0;

static void ring_push(char c)
{
    u32 next = (ring_head + 1) % RING_SZ;
    if (next == ring_tail)
        return;                       /* 满则丢弃，简单粗暴 */
    ring[ring_head] = c;
    ring_head = next;
}
static int ring_pop(char *out)
{
    if (ring_tail == ring_head)
        return 0;
    *out = ring[ring_tail];
    ring_tail = (ring_tail + 1) % RING_SZ;
    return 1;
}

/* ---- 阶段6①：给 usb_kbd.c 的几个导出 ----
   kbd_feed 是「两个生产者」版本：UART 中断和键盘轮询都会往同一个 ring 塞字节，
   所以塞之前必须关 IRQ（UART 中断可能在轮询线程塞到一半时插进来，把 head/tail 冲掉）。 */
void kbd_feed(char c)
{
    u64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2");      /* 关 IRQ */
    ring_push(c);
    __asm__ volatile("msr daif, %0" :: "r"(daif));
}
void kputs(const char *s)  { uart_puts(s); }
void kputhex(u64 v)        { uart_puthex(v); }
void kputdec(u32 v)        { uart_putdec(v); }

/* ---------------- GIC + 串口中断 初始化 ---------------- */
static void gic_init(void)
{
    u32 arch_lo = (GICD_PIDR2_V2 >> 4) & 0xF;        /* 0x2=GICv2, 0x3=GICv3 */
    u32 arch_hi = (GICD_PIDR2_V3 >> 4) & 0xF;
    gic_ver = (arch_hi == 3 || arch_lo == 3) ? 3 : 2;
    smp_set_gic_ver((int)gic_ver);      /* 告诉 smp.c：从核也按这个版本初始化 */

    if (gic_ver == 3) {
        /* 等本 CPU 的 redistributor 就绪（GICR_CTLR.RWP=bit31） */
        for (volatile int i = 0; i < 1000000 && (GICR_CTLR & (1u << 31)); i++) { }

        GICD_CTLR = 0;                               /* 先关分发 */
        GICD_CTLR = (1u << 4) | (1u << 1);           /* ARE_NS + EnableGrp1NS */

        /* SPI 33（串口）→ Group1 + 优先级 + 使能 */
        GICD_IGROUPR(UART0_IRQ) |= (1u << (UART0_IRQ % 32));
        GICD_IPRIORITYR(UART0_IRQ) = 0x80;
        GICD_ISENABLER(UART0_IRQ) = (1u << (UART0_IRQ % 32));

        /* CPU 接口走系统寄存器 */
        { u64 sre = 1; __asm__ volatile("msr ICC_SRE_EL1, %0" :: "r"(sre)); }
        __asm__ volatile("isb");
        { u64 pmr = 0xF0; __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(pmr)); }
        { u64 g1 = 1;   __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(g1)); }
        __asm__ volatile("isb");
    } else {
        GICD_CTLR = 0;                               /* 先关中断分发 */
        GICD_IPRIORITYR(UART0_IRQ) = 0x80;           /* 串口中断优先级 */
        GICD_ISENABLER(UART0_IRQ) = (1u << (UART0_IRQ % 32));  /* 使能这一路 */
        GICD_CTLR = 1;                               /* 开中断分发 */
        GICC_PMR = 0xFF;                             /* 接受所有优先级 */
        GICC_CTLR = 1;                               /* 开 CPU 接口 */
    }

    UART_IMSC = IMSC_RXIM;                           /* 只开 RX 掩码（赋值，勿 |=） */
    __asm__ volatile("msr daifclr, #2");             /* 开 CPU 中断（IRQ） */
}

/* ================= 阶段2：MMU ================= */

/* 页表描述符位 */
#define PTE_VALID      (1UL << 0)
#define PTE_TABLE      (1UL << 1)   /* 11=表/页描述符，01=块描述符（L1/L2） */
#define PTE_SH_INNER   (3UL << 8)
#define PTE_AF         (1UL << 10)  /* Access Flag：忘了置 1 必吃 fault！ */
/* AP[2:1]=bits[7:6]：00=EL1RW/EL0无权  01=EL1RW/EL0RW  10=EL1RO/EL0无权  11=EL1RO/EL0RO */
#define PTE_AP_KRW     (0UL << 6)
#define PTE_AP_KRW_URW (1UL << 6)
#define PTE_AP_KRO     (2UL << 6)
#define PTE_AP_KRO_URO (3UL << 6)
#define PTE_PXN        (1UL << 53)  /* EL1 不可执行 */
#define PTE_UXN        (1UL << 54)  /* EL0 不可执行 */
#define PTE_ATTR_NORM  (0UL << 2)   /* MAIR Attr0：Normal WB RA WA */
#define PTE_ATTR_DEV   (1UL << 2)   /* MAIR Attr1：Device-nGnRE */
#define PTE_ATTR_NC    (2UL << 2)   /* MAIR Attr2：Normal Non-Cacheable（帧缓冲用） */

#define SZ_4K   0x1000UL
#define SZ_2M   0x200000UL

/* 公共属性位（⚠️ 每种 prot 都必须含 VALID|TABLE|AF|SH，缺一位就是 L3 translation fault） */
#define PROT_DEV    (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_AP_KRW | PTE_PXN | PTE_UXN | PTE_ATTR_DEV)
#define PROT_RAM    (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_AP_KRW | PTE_PXN | PTE_UXN | PTE_ATTR_NORM)
#define PROT_TEXT   (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_AP_KRO | PTE_UXN | PTE_ATTR_NORM)
#define PROT_EL0TX  (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_AP_KRO_URO | PTE_PXN | PTE_ATTR_NORM)
#define PROT_RODATA (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_AP_KRO | PTE_PXN | PTE_UXN | PTE_ATTR_NORM)
/* 帧缓冲：Normal-**Cacheable**(WB) + EL1 读写。
   ⚠️ 三种属性实测对比（800x600，bench 命令，KVM + ramfb）：
     Device-nGnRE : 清屏 115ms / 滚一行 200ms（每个像素都强序）
     Normal-NC    : 清屏 447us / 滚一行 **70ms** ← 写快读慢，NC 读也是逐个怼 DRAM
     Normal-WB    : 这才是对的（见下方实测）
   为什么 WB 不需要 cache 维护：QEMU/KVM 里 guest 的 cacheable 内存就是宿主的 cacheable 内存，
   ramfb 显示端读的是同一份物理内存、同一个 cache 域 → 天然一致。
   （真机上若有真实扫描输出设备，才需要 dc civac；本项目只有 QEMU 虚拟显示。）
   另外 nc 属性宏 PTE_ATTR_NC / MAIR Attr2 保留，给以后真机可能需要的显存映射用。 */
#define PROT_FB_BLK (PTE_VALID | PTE_AF | PTE_SH_INNER | PTE_AP_KRW | PTE_PXN | PTE_UXN | PTE_ATTR_NORM)
/* ⚠️ L1/L2 的块（block）描述符绝不能带 PTE_TABLE（bit1=1 会被当成"指向下一级表的表描述符"！）
   块 = VALID(bit0=1, bit1=0) + 属性 */
#define PROT_DEV_BLK  (PROT_DEV  & ~PTE_TABLE)
#define PROT_RAM_BLK  (PROT_RAM  & ~PTE_TABLE)

/* —— 阶段5②：用户程序区（内核 2MB 区的顶部 0x401C0000-0x401FFFFF，恒等映射） ——
   放内核 2MB 区里是因为 l3_kern 只细分这一块，用户页直接复用 fill_l3；
   起点必须在 __bss_end 之后（fs_disk 内存盘 1MB 就在 bss 里，起点放 0x40100000 撞过；
   2026-10-24 加 CJK 字库后 __bss_end 涨到 0x401C1010，把 USER_BASE 从 0x401C0000
   提到 0x401D0000 才腾出空间——加字库/大数组后要重新看 __bss_end！
   2026-10-30 加网卡驱动 + 网络栈后 __bss_end 涨到 0x401E6400，USER_BASE 提到 0x401F0000），
   下面 mmu_init 有断言兜底；镜像再长就把 USER_BASE 往上挪（上限 0x40200000）。
   代码段=EL0 可执行可读（PXN，EL1 不执行用户代码）；data/栈=EL0 可读写（UXN） */
#define USER_BASE 0x40200000UL       /* 2026-10-30：用户区独立成 0x40200000-0x403FFFFF 的整个 2MB 块，
                                        有自己的 L3（l3_user），不再跟内核抢那个 2MB 块的顶部 64KB。
                                        这是给 fork/exec 做地址空间的地基：以后每个进程换自己的 L3 即可。
                                        内核 __bss_end 只增不减，永远别再让它顶到 USER_BASE 上。 */
#define USER_END  0x40400000UL
#define EL0_STACK_RESERVE 4096UL  /* 用户段不得进入的顶部保留区：EL0 栈从 USER_END 向下用这么多 */
/* 参数区：栈保留区下面一块，内核装载时写好 argc/argv/字符串，用户程序按固定 VA 读 */
#define UARGS_ADDR (USER_END - EL0_STACK_RESERVE - 512UL)
#define UARGS_MAX  8
#define PROT_USEREX (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_AP_KRO_URO | PTE_PXN | PTE_ATTR_NORM)
#define PROT_USERRW (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | PTE_AP_KRW_URW | PTE_PXN | PTE_UXN | PTE_ATTR_NORM)

/* 链接脚本给的节区边界（linker.ld） */
extern u64 __text_start[], __text_end[];
extern u64 __el0text_start[], __el0text_end[];
extern u64 __rodata_start[], __rodata_end[];
extern u64 __data_start[], __bss_end[];
extern u64 _stack_top[];

/* 页表（4KB 对齐，放在 .bss 里，随内核一起被映射为 RW） */
__attribute__((aligned(4096))) static u64 l0_table[512];  /* 阶段6①：48 位 VA 的 L0（只有 [0] 有用） */
__attribute__((aligned(4096))) static u64 l1_table[512];  /* 管 512×1GB */
__attribute__((aligned(4096))) static u64 l2_dev[512];    /* 第0个GB：设备 */
__attribute__((aligned(4096))) static u64 l2_ram[512];    /* 第1个GB：RAM 0x40000000+ */
__attribute__((aligned(4096))) static u64 l3_kern[512];   /* 内核所在 2MB：细分成 4KB 页 */
__attribute__((aligned(4096))) static u64 l3_user[512];   /* 用户区 0x40200000-0x403FFFFF 的 L3（fork 时整页复制这个表就换地址空间） */
__attribute__((aligned(4096))) static u64 l2_pcie_hi[512];/* 第257个GB：PCIe ECAM(0x4010000000) */
__attribute__((aligned(4096))) static u64 l1_hi[512];     /* 阶段6②：64 位 MMIO 窗口（L0[1] → 0x8000000000 起 256GB） */

/* EL0 演示页：独占一页（数组 4KB 对齐 + 4KB 大小），页表单独标 EL0 可读写 */
__attribute__((aligned(4096))) static volatile u8 el0_page[4096];
/* EL0 演示专用栈（2026-10-28 修：原来拿 el0_page 顶部当栈——数据页和栈挤在同一页，
   演示代码多写几个变量就会互踩；现在各占一页，映射见 mmu_init） */
__attribute__((aligned(4096))) static u8 el0_stack[4096];

/* 把 [start,end) 的 4KB 页填进 L3（VA=PA 恒等）
   ⚠️ 起止必须各自对齐到页：__data_start 之类是 0x...4c0 这种值，
   直接 `a | prot` 会把 PA 低 12 位拼进描述符 → AP 位被污染成只读 → MMU 一开全线写故障 */
/* 按地址落到对应的 L3：内核 2MB 块用 l3_kern，用户 2MB 块用 l3_user */
static void fill_pt(u64 start, u64 end, u64 prot)
{
    start &= ~(SZ_4K - 1);
    end = (end + SZ_4K - 1) & ~(SZ_4K - 1);
    for (u64 a = start; a < end; a += SZ_4K) {
        if (a >= USER_BASE && a < USER_END) l3_user[(a & (SZ_2M - 1)) >> 12] = a | prot;
        else                                l3_kern[(a & (SZ_2M - 1)) >> 12] = a | prot;
    }
}

static void mmu_init(void)
{
    /* --- L0：48 位 VA 的入口（T0SZ=16）；所有映射都落在 L0[0] 下 --- */
    l0_table[0] = (u64)l1_table | PTE_VALID | PTE_TABLE;

    /* --- L1：第0个GB → 设备表；第1个GB → RAM 表 --- */
    l1_table[0] = (u64)l2_dev | PTE_VALID | PTE_TABLE;
    l1_table[1] = (u64)l2_ram | PTE_VALID | PTE_TABLE;

    /* --- L2 设备：GIC(0x08000000) 一块、UART(0x09000000) 一块；空隙不映射 --- */
    l2_dev[64] = 0x08000000UL | PROT_DEV_BLK;   /* 64×2MB = 0x08000000：GICD+GICC */
    l2_dev[72] = 0x09000000UL | PROT_DEV_BLK;   /* 72×2MB = 0x09000000：PL011 */

    /* --- 阶段6①：PCIe 窗口（QEMU virt）
           0x10000000-0x3effffff = 32 位 MMIO 窗口（xHCI 寄存器 BAR 在这里）
           0x4010000000-0x401fffffff = ECAM（256MB，256 条 bus）
           全部按 Device 属性映射；这一段 l2_dev 原本是空的，不会跟 GIC/UART 打架 --- */
    for (u64 a = 0x10000000UL; a < 0x3F000000UL; a += SZ_2M)
        l2_dev[(a >> 21) & 0x1FF] = a | PROT_DEV_BLK;

    /* --- 阶段6②：64 位 MMIO 窗口（UEFI 会把 64 位 BAR 放到 0x8000000000 以上） --- */
    l0_table[1] = (u64)l1_hi | PTE_VALID | PTE_TABLE;
    for (u32 i = 0; i < 512; i++)
        l1_hi[i] = (0x8000000000UL + (u64)i * 0x40000000UL) | PROT_DEV_BLK;

    /* --- 阶段6③：把 4GB~256GB 的巨型窗口也按 Device 映射 ---
       UEFI 固件会把 64 位 BAR（virtio 的 modern 配置区常见）放到 4GB 以上；
       只映射 0~1GB 时，驱动读到的特征是垃圾值（表现为"设备不支持 VIRTIO 1.0"）。
       L1 索引 2..255 = 0x80000000..0x3FFFFFFF 区间的 1GB 块；索引 0 是设备、1 是 RAM、
       256 是 ECAM，都不动。 */
    for (int i = 2; i < 256; i++)
        l1_table[i] = ((u64)i * 0x40000000ULL) | PROT_DEV_BLK;

    /* ECAM 在 0x4010000000：L1 索引 256 管的是 0x4000000000 起的 1GB，
       ECAM 是其中第 (0x4010000000-0x4000000000)/2MB = 128 个 2MB 块 —— 别从 0 开始填！ */
    l1_table[256] = (u64)l2_pcie_hi | PTE_VALID | PTE_TABLE;
    /* ⚠️ 2026-10-25：只映射 ECAM 的 128..255 块不够 —— PCIe 根端口后面的 virtio 设备，
       EDK2 会把 BAR 分配到 0x4010xxxxxx 附近甚至更高，落在窗口外一访问就数据中止死循环
       （FAR_EL1=0x4010300010 实测）。这里把 L1[256] 管的整 1GB（0x4000000000~0x403FFFFFFF）
       全部按设备内存映射，覆盖 ECAM + 高位 BAR。 */
    for (int i = 0; i < 512; i++)
        l2_pcie_hi[i] = (0x4000000000UL + (u64)i * SZ_2M) | PROT_DEV_BLK;

    /* --- L2 RAM：块0(0x40000000-0x401FFFFF) 下钻 L3 细分内核权限；
           其余 63 块 2MB 大页 RW（RAM 到 0x48000000 共 128MB） --- */
    l2_ram[0] = (u64)l3_kern | PTE_VALID | PTE_TABLE;
    for (int i = 0; i < 512; i++) l3_user[i] = 0;
    /* ⚠️ 整段 1GB 都按 Normal-WB 映射（原来只到 128MB）：
       UEFI 分配的 GOP 帧缓冲可能落在 0x48000000 以上（实测 0x4c430000），
       没映射就会吃 translation fault/自愈，屏幕画不出来。 */
    for (int i = 1; i < 512; i++)
        l2_ram[i] = (0x40000000UL + (u64)i * SZ_2M) | PROT_RAM_BLK;
    /* ⚠️ 必须放在上面这个「批量填 2MB 大页」之后，否则会被它覆盖回大页
       （大页带 UXN，用户代码一取指就是 2 级权限故障，FAR=入口地址） */
    l2_ram[1] = (u64)l3_user | PTE_VALID | PTE_TABLE;

    /* --- L3（内核 2MB 区）按节区上权限：
           0x40000000-0x4007FFFF 保留区保持未映射（误访问=真 fault）
           text：EL1 只读可执行、EL0 不可执行
           el0text：EL0 可执行可读、EL1 不执行（PXN）
           rodata：EL1 只读、双方不可执行
           data/bss/栈/页表：EL1 可读写、双方不可执行
           el0_page：EL0 也给读写 --- */
    fill_pt((u64)__text_start,     (u64)__text_end,     PROT_TEXT);
    fill_pt((u64)__el0text_start,  (u64)__el0text_end,  PROT_EL0TX);
    fill_pt((u64)__rodata_start,   (u64)__rodata_end,   PROT_RODATA);
    fill_pt((u64)__data_start,     (u64)__bss_end,      PROT_RAM);
    /* --- 帧缓冲：按 2MB 块补映射（Device 属性；内核所在的首块跳过） --- */
    if (g_bi && g_bi->magic == BOOTINFO_MAGIC && g_bi->base && g_bi->size) {
        u64 s0 = g_bi->base & ~(SZ_2M - 1);
        u64 s1 = (g_bi->base + g_bi->size + SZ_2M - 1) & ~(SZ_2M - 1);
        for (u64 a = s0; a < s1; a += SZ_2M) {
            u32 idx = (u32)((a >> 21) & 0x1FF);
            if (a < 0x40000000UL)
                l2_dev[idx] = a | PROT_DEV_BLK;
            else if (idx != 0)
                l2_ram[idx] = a | PROT_DEV_BLK;
        }
    }

    fill_pt((u64)&el0_page[0],     (u64)&el0_page[0] + SZ_4K,
            PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER |
            PTE_AP_KRW_URW | PTE_PXN | PTE_UXN | PTE_ATTR_NORM);
    fill_pt((u64)&el0_stack[0],    (u64)&el0_stack[0] + SZ_4K,
            PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER |
            PTE_AP_KRW_URW | PTE_PXN | PTE_UXN | PTE_ATTR_NORM);

    /* 阶段5②：用户程序区整段先给 EL0 可读写（loader 再把代码段改成可执行）。
       顺手断言内核镜像没长进用户区——静态数组一多最容易撞上 */
    if ((u64)__bss_end > USER_BASE) {
        uart_puts("[!!!] 内核镜像撞进用户区 USER_BASE="); uart_puthex(USER_BASE); uart_puts("，停机\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    fill_pt(USER_BASE, USER_END, PROT_USERRW);

    /* --- 帧缓冲：按 bootinfo 把对应 2MB 块改成 Normal-NC ---
       fb_init() 在 mmu_init() 之前跑，g_bi 已经有值。
       落点可能在：0x40000000-0x7FFFFFFF（RAM 窗口）或 0x10000000-0x3EFFFFFF（32 位 MMIO 窗口）。 */
    if (g_bi && g_bi->magic == BOOTINFO_MAGIC && g_bi->base) {
        u64 sz = g_bi->size ? (u64)g_bi->size
                            : (u64)(g_bi->stride ? g_bi->stride : g_bi->width) * g_bi->height * 4;
        u64 s0 = (u64)g_bi->base & ~(SZ_2M - 1);
        u64 e0 = ((u64)g_bi->base + sz + SZ_2M - 1) & ~(SZ_2M - 1);
        for (u64 a = s0; a < e0; a += SZ_2M) {
            if (a >= 0x40000000UL && a < 0x80000000UL)
                l2_ram[(a >> 21) & 0x1FF] = a | PROT_FB_BLK;
            else if (a >= 0x10000000UL && a < 0x3F000000UL)
                l2_dev[(a >> 21) & 0x1FF] = a | PROT_FB_BLK;
        }
    }

    /* --- 系统寄存器：MAIR / TCR / TTBR0 --- */
    /* MAIR: Attr0=0xFF Normal WB RA WA；Attr1=0x04 Device-nGnRE */
    u64 mair = (0xFFUL << 0) | (0x04UL << 8) | (0x44UL << 16);   /* Attr2 = Normal-NC */
    /* TCR: IPS=3(40bit PA) | EPD1=1(禁TTBR1) | SH0=inner | ORGN0/IRGN0=WBWA | T0SZ=16(48bit VA，从 L0 开始) | TG0=4KB
       ⚠️ T0SZ 决定起始查表级：16=48bit 从 L0 开始（要 L0 表）；25=39bit 从 L1 开始
       ⚠️ 阶段6①踩过：39 位 VA 只有 0-256GB，PCIe ECAM 在 0x4010000000（=256GB+16MB）
          正好越界 → 访问必吃 translation fault。改 48 位后 L0[0]→l1_table→l2_pcie_hi 即可。 */
    u64 tcr  = (3UL << 32) | (1UL << 23) | (3UL << 12) | (1UL << 10) | (1UL << 8) | 16UL;
    __asm__ volatile("msr mair_el1,  %0" :: "r"(mair));
    __asm__ volatile("msr tcr_el1,   %0" :: "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((u64)l0_table));

    /* --- 翻译缓存：冲 TLB 再开 MMU --- */
    __asm__ volatile("dsb sy");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    /* --- 开 MMU：SCTLR_EL1 的 M(0) + 数据 cache C(2) + 指令 cache I(12) --- */
    u64 sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");   /* 恒等映射：下一条指令同址继续执行，安全切换 */
}

static void mmu_status(void)
{
    u64 sctlr, tcr, ttbr, mair;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    __asm__ volatile("mrs %0, tcr_el1"   : "=r"(tcr));
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    __asm__ volatile("mrs %0, mair_el1"  : "=r"(mair));
    uart_puts("\n[mmu] SCTLR="); uart_puthex(sctlr);
    uart_puts(" (M="); uart_puthex(sctlr & 1); uart_puts(" C="); uart_puthex((sctlr >> 2) & 1);
    uart_puts(" I="); uart_puthex((sctlr >> 12) & 1); uart_puts(")\n");
    uart_puts("[mmu] TCR  ="); uart_puthex(tcr);
    uart_puts("  TTBR0="); uart_puthex(ttbr);
    uart_puts("  MAIR="); uart_puthex(mair); uart_puts("\n");
    uart_puts("[sect] text "); uart_puthex((u64)__text_start); uart_puts(".."); uart_puthex((u64)__text_end);
    uart_puts("\n[sect] el0  "); uart_puthex((u64)__el0text_start); uart_puts(".."); uart_puthex((u64)__el0text_end);
    uart_puts("\n[sect] roda "); uart_puthex((u64)__rodata_start); uart_puts(".."); uart_puthex((u64)__rodata_end);
    uart_puts("\n[sect] data "); uart_puthex((u64)__data_start); uart_puts(".."); uart_puthex((u64)__bss_end);
    uart_puts("\n[el0]  page "); uart_puthex((u64)&el0_page[0]); uart_puts("\n");
}

/* ---------------- ARM Generic Timer（物理定时器 PPI=INTID 30，阶段3） ---------------- */
#define TIMER_IRQ 30
volatile u64 tick = 0;      /* 非 static：fs.c 用 extern 读它算 mtime（秒 = tick/10） */
static u64 timer_interval = 0;

static void timer_init(u32 hz)
{
    u64 freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0)
        freq = 62500000UL;                        /* QEMU virt 兜底 62.5MHz */
    timer_interval = freq / hz;
    if (gic_ver == 3) {
        /* PPI30 挂在 redistributor 上（GICD 的 ISENABLER0 管不到 PPI） */
        for (volatile int i = 0; i < 1000000 && (GICR_CTLR & (1u << 31)); i++) { }
        GICR_IGROUPR0  |= (1u << TIMER_IRQ);          /* Group1 */
        GICR_IPRIORITYR(TIMER_IRQ) = 0x80;
        GICR_ISENABLER0 = (1u << TIMER_IRQ);          /* 使能 PPI30 */
    } else {
        GICD_IPRIORITYR(TIMER_IRQ) = 0x80;            /* PPI30 优先级 */
        GICD_ISENABLER(TIMER_IRQ)  = (1u << TIMER_IRQ);  /* ISENABLER 第1组 bit30 */
    }
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval));
    __asm__ volatile("msr cntp_ctl_el0, %0"  :: "r"(1UL));  /* enable + 不屏蔽 */
}

static void timer_rearm(void)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval));
}

/* ---------------- 任务：内核线程，各占一条栈，靠异常帧指针互换实现抢占 ---------------- */
struct task {
    u64 frame;              /* 被抢占时保存的异常帧地址（在它自己的栈上） */
    u64 stack_top;
    const char *name;
    u64 switches;           /* 被调度到的次数 */
};
#define NTASK 2
static struct task tasks[NTASK];
static int cur_task = 0;

/* task1 的栈（.bss，8KB） */
__attribute__((aligned(16))) static u8 t1_stack[8192];

/* 造一个"初始异常帧"，RESTORE_CTX 一恢复就等于从异常返回进 fn */
static void task_create(int i, void (*fn)(void), const char *name)
{
    u64 fp = tasks[i].stack_top - 288;            /* 288=帧大小（SAVE_CTX 口径） */
    u64 *p = (u64 *)fp;
    for (int k = 0; k < 35; k++)
        p[k] = 0;                                 /* x0..x30 + elr/spsr/esr/far 清零 */
    p[31] = (u64)fn;                              /* ELR @248 → 一恢复就跳进 fn */
    p[32] = 0x345;                                /* SPSR @256 → EL1h、D/A/F 屏蔽、IRQ 开 */
    tasks[i].frame = fp;
    tasks[i].name = name;
}

/* 把帧换成"下一个任务"的；调用者（irq_entry）会从换后的帧 RESTORE */
static struct regs *task_switch(struct regs *f)
{
    int next = (cur_task + 1) % NTASK;
    tasks[cur_task].frame = (u64)f;               /* 当前任务的现场记在它自己的栈上 */
    cur_task = next;
    tasks[cur_task].switches++;
    return (struct regs *)tasks[cur_task].frame;
}

/* EL0 看门狗用到的三个符号（都定义在后面的加载器一节；文件作用域声明，C 合法） */
static int  user_running;      /* 用户程序跑在 EL0 期间 = 1（定义处带初始化） */
static u64  el0_enter_tick;    /* 进 EL0 时的 tick 快照（看门狗计时起点） */
static void el1_landing(void); /* eret 着陆点：exit / fault / 强杀都回这重新接管 shell */
#define EL0_TIMEOUT_TICKS 300u /* tick = 10Hz → EL0 最长 30 秒不进系统调用 */

/* ---------------- IRQ 处理：GIC 认路 → 串口收字节 / 定时器推进时钟 + 调度 ---------------- */
struct regs *handle_irq(struct regs *f)
{
    u32 iar = (gic_ver == 3) ? (u32)icc_iar1_read() : GICC_IAR;
    u32 id  = iar & 0x3FF;
    if (id == 1023)                    /* 伪中断 */
        return f;
    if (id == UART0_IRQ) {
        while (!(UART_FR & FR_RXFE))   /* 把串口 FIFO 里积压的全收走 */
            ring_push((char)(UART_DR & 0xFF));
    } else if (id == TIMER_IRQ) {
        timer_rearm();                 /* 重装计数 = 清挂起 */
        tick++;
        /* EL0 看门狗（2026-10-28 修）：原来 enter_el0_at 把 DAIF 全屏蔽，EL0 里死循环
           连中断都进不来 → 整机挂死只能重启。现在 EL0 开着 IRQ，这里才有机会插手：
           用户程序 30 秒没进任何系统调用就强杀回着陆点，shell 立刻回来。 */
        if (user_running && (f->spsr & 0xF) == 0 &&
            (u32)(tick - el0_enter_tick) > EL0_TIMEOUT_TICKS) {
            uart_puts("\n[run] EL0 看门狗：程序 30 秒没有任何系统调用，强杀回 shell\n");
            user_running = 0;
            f->spsr = 0x345;
            f->elr  = (u64)el1_landing;
        }
        f = task_switch(f);            /* 抢占：换到下一个任务的帧 */
    }
    if (id >= 16) {                    /* PPI(30) 和 SPI(33) 都要 EOI；SGI 0~15 我们不用 */
        if (gic_ver == 3) icc_eoir1_write(iar);
        else              GICC_EOIR = iar;
    }
    return f;
}

/* task1：干活的家伙——每 10 片（约 1 秒）打一次心跳，其余时间 wfi 睡觉
   ⚠️ 计数要用"自己跑了几片"，别用 tick%10：严格轮转下 task1 永远只看到奇数 tick，
   tick% 100 == 0 永远不成立（第一版心跳一次都不打就是栽在这） */
static void __attribute__((noreturn)) task1_main(void)
{
    u64 seen = 0, slices = 0;
    for (;;) {
        __asm__ volatile("wfi");
        u64 t = tick;
        if (t != seen) {
            seen = t;
            slices++;
            if (slices % 100 == 0 && !redir_on) {   /* 重定向期间闭嘴，别把心跳写进文件 */
                /* 用 hb_*（只走串口）：真机屏幕不刷，shell 才看得清 */
                hb_puts("[task1] 我在干活（每 10 秒一行）  tick=");
                hb_puthex(t);
                hb_puts("  我已跑 ");
                hb_puthex(slices);
                hb_puts(" 片\n");
            }
        }
    }
}

static void show_tasks(void)
{
    uart_puts("\n[tick] 现在 tick="); uart_puthex(tick);
    uart_puts("  当前任务=task"); uart_puthex((u64)cur_task); uart_puts("\n");
    uart_puts("任务表（共 "); uart_putdec(NTASK); uart_puts(" 个）:\n");
    for (int i = 0; i < NTASK; i++) {
        uart_puts("[task] task"); uart_puthex((u64)i);
        uart_puts(" "); uart_puts(tasks[i].name);
        uart_puts("  被调度 "); uart_puthex(tasks[i].switches);
        uart_puts(" 次  帧="); uart_puthex(tasks[i].frame);
        if (i == (int)cur_task) {
            /* 运行中任务的旧帧指针指向已被自身后续栈使用覆盖的区域（帧恢复时就消费掉了），
               读它的 ELR 只会看到垃圾——显示"运行中"更诚实 */
            uart_puts("（运行中，上次帧已消费）");
        } else if (tasks[i].frame) {
            struct regs *fr = (struct regs *)tasks[i].frame;
            uart_puts("  上次抢占点 ELR="); uart_puthex(fr->elr);
        }
        uart_puts("\n");
    }
}

/* ---------------- ESR 异常类解码（EC 字段 = ESR[31:26]） ---------------- */
static const char *ec_name(u64 ec)
{
    switch (ec) {
    case 0x00: return "未知指令(UDF/未定义)";
    case 0x15: return "SVC 系统调用";
    case 0x20: case 0x21: return "取指中止(取不到指令)";
    case 0x22: case 0x23: return "PC 未对齐";
    case 0x24: case 0x25: return "数据中止(读写坏地址/权限违规)";
    case 0x3C: return "BRK 断点指令";
    default:   return "其他类型";
    }
}

/* 阶段5③：cwd 路径解析（定义在下面 shell 段；run 也要用所以先声明） */
static int path_resolve(const char *in, char *out, int cap);

/* ---------------- 阶段5②：ELF 加载器 + 用户程序系统调用 ---------------- */
/* 用户程序由 build.sh 编成独立 ELF（-Ttext=0x40100000），再用 objcopy -I binary
   把 ELF 文件本身嵌进内核镜像（符号 _binary_<名>_start/_end），开机写进 /bin */
extern const u8 _binary_hello_elf_start[], _binary_hello_elf_end[];
extern const u8 _binary_bad_elf_start[],  _binary_bad_elf_end[];
extern const u8 _binary_hang_elf_start[], _binary_hang_elf_end[];
extern const u8 _binary_cat_elf_start[],  _binary_cat_elf_end[];
extern const u8 _binary_args_elf_start[], _binary_args_elf_end[];

static u8 elf_buf[65536];           /* 装载缓冲：用户程序最大 64KB */
static int user_running = 0;        /* 用户程序跑在 EL0 期间=1（SVC/fault 走用户路径） */

/* ELF64 头/程序头（按 AArch64 ELF ABI 手写，不依赖工具链头文件；尺寸 64/56 字节） */
struct elf64_ehdr {
    u8  magic[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};
#define PT_LOAD 1
#define PF_X    1

/* 把编译期嵌进内核的文件播种进 /bin：fs_createfile 对已存在的名字返回 -1
   → 已存在就不覆盖（磁盘上被改过的文件重启后原样保留，不是每次开机重置） */
static void seed_one(const char *path, const u8 *s, const u8 *e)
{
    int ino = fs_createfile(path);
    if (ino > 0)
        fs_setfile(ino, s, (u32)(e - s));
}
static void seed_userprogs(void)
{
    seed_one("/bin/hello", _binary_hello_elf_start, _binary_hello_elf_end);
    seed_one("/bin/cat",   _binary_cat_elf_start,   _binary_cat_elf_end);
    seed_one("/bin/args",  _binary_args_elf_start,  _binary_args_elf_end);
    seed_one("/bin/bad",   _binary_bad_elf_start,  _binary_bad_elf_end);
    seed_one("/bin/hang",  _binary_hang_elf_start, _binary_hang_elf_end);
}

/* 写完代码冲 I-cache：DC CVAU（按行清数据缓存）→ DSB → IC IVAU → DSB → ISB。
   QEMU TCG 不模拟 I-cache 看不出差别，真机会执行到陈旧指令——这步不能省 */
static void icache_flush(u64 start, u64 end)
{
    for (u64 a = start & ~63UL; a < end; a += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish");
    for (u64 a = start & ~63UL; a < end; a += 64)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish; isb");
}

/* eret 进 EL0 的通用入口（ELR=入口、SP_EL0=用户栈顶、SPSR=EL0t 全屏蔽） */
static void __attribute__((noreturn)) enter_el0_at(u64 elr, u64 sp0);
static void __attribute__((noinline)) el1_landing(void);   /* 着陆点：exit/fault 时 eret 回内核重新接管 shell */

static void run_userprog(const char *path, const char *argline)
{
    char rp[128];
    if (!path || !*path) { uart_puts("用法: run <路径>（如 run /bin/hello）\n"); return; }
    if (path_resolve(path, rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
    path = rp;                                /* 阶段5③：之后全用规范绝对路径 */
    int ino = fs_lookup(path);
    if (ino < 0)  { uart_puts("没有这个文件: "); uart_puts(path); uart_puts("\n"); return; }
    int n = fs_readfile(ino, elf_buf, sizeof(elf_buf));
    if (n < 0)    { uart_puts("读文件失败（权限不够？）\n"); return; }
    if (n < 64)   { uart_puts("太小了，不像 ELF\n"); return; }

    struct elf64_ehdr *eh = (struct elf64_ehdr *)elf_buf;
    if (eh->magic[0] != 0x7F || eh->magic[1] != 'E' ||
        eh->magic[2] != 'L' || eh->magic[3] != 'F') {
        uart_puts("不是 ELF 文件\n"); return;
    }
    if (eh->phoff + (u64)eh->phnum * eh->phentsize > (u64)n) { uart_puts("ELF 程序头越界\n"); return; }
    if (eh->entry < USER_BASE || eh->entry >= USER_END)       { uart_puts("入口不在用户区，拒跑\n"); return; }

    /* ⚠️ 先把整个用户区恢复 EL1 可写：上一个程序的代码段是 PROT_USEREX（EL1 只读！），
       不恢复的话这次装载的 fs_memcpy 会从 EL1 写权限故障（实测：FAR 逐 +8 的异常风暴） */
    fill_pt(USER_BASE, USER_END, PROT_USERRW);
    __asm__ volatile("tlbi vmalle1; dsb ish; isb");

    for (int i = 0; i < eh->phnum; i++) {
        struct elf64_phdr *ph = (struct elf64_phdr *)(elf_buf + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        /* 除越界/回绕外，段顶还要给用户栈让出 EL0_STACK_RESERVE：sp 初始在 USER_END 向下长，
           段一直顶到 USER_END 的话第一条 push 就踩自己的 .bss（2026-10-28 修） */
        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz > USER_END - EL0_STACK_RESERVE ||
            ph->vaddr + ph->memsz < ph->vaddr) { uart_puts("段地址越界，拒跑\n"); return; }
        if (ph->filesz > ph->memsz)           { uart_puts("filesz>memsz，拒跑\n"); return; }
        if (ph->offset > (u64)n || ph->filesz > (u64)n - ph->offset)
            { uart_puts("段文件越界\n"); return; }
        u8 *dst = (u8 *)ph->vaddr;
        fs_memcpy(dst, elf_buf + ph->offset, (u32)ph->filesz);
        for (u64 k = ph->filesz; k < ph->memsz; k++)
            dst[k] = 0;                                              /* bss 清零 */
        fill_pt(ph->vaddr, ph->vaddr + ph->memsz,
                (ph->flags & PF_X) ? PROT_USEREX : PROT_USERRW);     /* 只有代码段给执行 */
        icache_flush(ph->vaddr, ph->vaddr + ph->memsz);
    }
    __asm__ volatile("tlbi vmalle1; dsb ish; isb");                  /* 页权限变了，冲 TLB */

    /* 参数区：argc / argv[] / 字符串池（用户程序按固定 VA 读，见 user.h 的 u_argc/u_argv） */
    {
        u64 *u = (u64 *)UARGS_ADDR;
        char *pool = (char *)(UARGS_ADDR + 8 * (UARGS_MAX + 1));
        int argc = 0;
        {   int k = 0;
            while (path[k] && k < 63) { pool[k] = path[k]; k++; }
            pool[k] = 0;
            u[1] = (u64)pool; argc = 1; pool += k + 1;
        }
        const char *q = argline;
        while (q && *q && argc < UARGS_MAX) {
            while (*q == ' ') q++;
            if (!*q) break;
            int k = 0;
            while (*q && *q != ' ' && k < 63) pool[k++] = *q++;
            pool[k] = 0;
            u[1 + argc] = (u64)pool;
            argc++;
            pool += k + 1;
        }
        u[0] = (u64)argc;
    }

    uart_puts("[run] 装载 "); uart_puts(path);
    uart_puts(" entry="); uart_puthex(eh->entry);
    uart_puts(" → 进 EL0（syscall: x8=0 write/2 open/3 read/4 close/5 lseek · x8=1 exit）\n");
    user_running = 1;
    enter_el0_at(eh->entry, USER_END);                               /* 一去不复返，exit/fault 回着陆点 */
}

/* SVC 系统调用分发（用户程序 EL0 → EL1）：x8=号，x0/x1=参数，返回值写回 x0 */
/* ---- 阶段8：POSIX 文件接口（fd 表；open 时把整份文件读进内核缓冲，上限 512 字节）---- */
#define NFDS   6
#define NFDBUF 512
static struct { int ino; int used; u32 off; u32 len; } g_fd[NFDS];
static u8 g_fdbuf[NFDS][NFDBUF];

/* 把用户态字符串拷进内核缓冲：先卡指针范围，再找 NUL，最多 max-1 字节 */
static int u_strcpy(char *dst, u64 uptr, int max)
{
    if (uptr < USER_BASE || uptr >= USER_END) return -1;
    for (int i = 0; i < max - 1; i++) {
        char c = ((const char *)uptr)[i];
        dst[i] = c;
        if (!c) return i;
    }
    dst[max - 1] = 0;
    return max - 1;
}

static void svc_dispatch(struct regs *f)
{
    if (f->x[8] == 0) {                       /* write：两种用法 */
        /* 老接口 write(ptr,len)：x0 落在用户区就当指针（/bin/hello 等老程序靠这个） */
        if (f->x[0] >= USER_BASE) {
            u64 p = f->x[0], len = f->x[1];
            if (p + len < p || p + len > USER_END || len > 65536) {
                uart_puts("\n[run] 用户指针越界，拒绝: p="); uart_puthex(p);
                uart_puts(" len="); uart_puthex(len); uart_puts("\n");
                f->x[0] = (u64)-1;
                return;
            }
            for (u64 i = 0; i < len; i++)
                uart_putc((char)((const u8 *)p)[i]);
            f->x[0] = len;
            return;
        }
        /* POSIX 风格 write(fd,buf,len)：只支持 fd=1/2（串口） */
        {
            u64 fd = f->x[0], p = f->x[1], len = f->x[2];
            if (fd != 1 && fd != 2) { f->x[0] = (u64)-1; return; }
            if (p < USER_BASE || p + len < p || p + len > USER_END || len > 65536) {
                f->x[0] = (u64)-1; return;
            }
            for (u64 i = 0; i < len; i++)
                uart_putc((char)((const u8 *)p)[i]);
            f->x[0] = len;
            return;
        }
    }
    if (f->x[8] == 2) {                       /* open(path, flags) → fd */
        char pbuf[128], rp[128];
        if (u_strcpy(pbuf, f->x[0], sizeof pbuf) < 0) { f->x[0] = (u64)-1; return; }
        if (path_resolve(pbuf, rp, sizeof rp) < 0)   { f->x[0] = (u64)-1; return; }
        int ino = fs_lookup(rp);
        if (ino < 0) { f->x[0] = (u64)-1; return; }
        int fd = -1;
        for (int i = 3; i < NFDS; i++) if (!g_fd[i].used) { fd = i; break; }
        if (fd < 0) { f->x[0] = (u64)-1; return; }
        int n = fs_readfile(ino, g_fdbuf[fd], NFDBUF);
        if (n < 0) { f->x[0] = (u64)-1; return; }
        g_fd[fd].ino = ino; g_fd[fd].used = 1; g_fd[fd].off = 0; g_fd[fd].len = (u32)n;
        f->x[0] = (u64)fd;
        return;
    }
    if (f->x[8] == 3) {                       /* read(fd, buf, len) */
        u64 fd = f->x[0], p = f->x[1], len = f->x[2];
        if (fd < 3 || fd >= NFDS || !g_fd[fd].used) { f->x[0] = (u64)-1; return; }
        if (p < USER_BASE || p + len < p || p + len > USER_END) { f->x[0] = (u64)-1; return; }
        u32 avail = g_fd[fd].len - g_fd[fd].off;
        u32 n = (u32)len < avail ? (u32)len : avail;
        for (u32 i = 0; i < n; i++) ((u8 *)p)[i] = g_fdbuf[fd][g_fd[fd].off + i];
        g_fd[fd].off += n;
        f->x[0] = n;
        return;
    }
    if (f->x[8] == 4) {                       /* close(fd) */
        u64 fd = f->x[0];
        if (fd < 3 || fd >= NFDS || !g_fd[fd].used) { f->x[0] = (u64)-1; return; }
        g_fd[fd].used = 0; g_fd[fd].ino = 0; g_fd[fd].off = g_fd[fd].len = 0;
        f->x[0] = 0;
        return;
    }
    if (f->x[8] == 5) {                       /* lseek(fd, off, whence) */
        u64 fd = f->x[0]; long off = (long)f->x[1]; u64 whence = f->x[2];
        if (fd < 3 || fd >= NFDS || !g_fd[fd].used) { f->x[0] = (u64)-1; return; }
        long base = whence == 0 ? 0 : (whence == 1 ? (long)g_fd[fd].off : (long)g_fd[fd].len);
        long npos = base + off;
        if (npos < 0) npos = 0;
        if ((u32)npos > g_fd[fd].len) npos = (long)g_fd[fd].len;
        g_fd[fd].off = (u32)npos;
        f->x[0] = (u64)npos;
        return;
    }
    if (f->x[8] == 1) {                                    /* exit：体面结束用户程序 */
        uart_puts("\n[run] 程序退出 code="); uart_putdec((u32)f->x[0]); uart_puts("\n");
        user_running = 0;
        f->spsr = 0x345;                                   /* 回 EL1h */
        f->elr  = (u64)el1_landing;                        /* 着陆函数重新接管 shell */
        return;
    }
    uart_puts("\n[run] 未知系统调用 x8="); uart_puthex(f->x[8]); uart_puts("（返回-1）\n");
    f->x[0] = (u64)-1;
}

static char *next_word(char **pp);          /* 定义在联网命令那一片，这里先用 */

static void cmd_run(char *a)
{
    char *p = a;
    char *path = next_word(&p);
    run_userprog(path ? path : "", p ? p : "");
}

/* ---------------- EL0 演示（.el0text 独占页） ---------------- */
/* 前向声明：SVC 落地函数（eret 目标，不返回任何人） */
static void __attribute__((noinline)) el1_landing(void);

__attribute__((section(".el0text"), noinline, aligned(4096)))
static void el0_demo(void)
{
    el0_page[0] = 0xAB;                              /* 合法：写自己的页 */
    volatile u64 *kern = (volatile u64 *)0x40080000UL;
    u64 v = *kern;                                   /* 非法：读内核 text(AP=EL1独占) → 权限故障 →
                                                        EL1 转储现场 → ELR+4 自愈 → 回 EL0 继续 */
    (void)v;
    el0_page[1] = (u8)(el0_page[0] + 1);             /* 合法：自愈后确认还活着 → 0xAC */
    __asm__ volatile("svc #0");                      /* 系统调用：体面地退出 EL0 */
    for (;;) { }
}

static void __attribute__((noreturn)) enter_el0_at(u64 elr, u64 sp0)
{
    __asm__ volatile("msr elr_el1, %0"  :: "r"(elr));
    /* ⚠️ 不写 spsr 的话 eret 会带着旧 SPSR 留在 EL1；
       0x340 = EL0t + 开 IRQ + 屏蔽 D/A/F（旧值 0x3c0 连 IRQ 也屏蔽了 → EL0 里死循环
       = 整机挂死；开 IRQ 后 handle_irq 的看门狗才有机会强杀回来）。 */
    __asm__ volatile("msr spsr_el1, %0" :: "r"(0x340UL));
    __asm__ volatile("msr sp_el0, %0"   :: "r"(sp0));
    el0_enter_tick = tick;                           /* 看门狗计时起点 */
    __asm__ volatile("eret");                              /* 一去不复返（SVC/exit/超时 回着陆点） */
    for (;;) { }                                           /* 给编译器兜底：eret 后确实回不来 */
}

static void enter_el0(void)
{
    enter_el0_at((u64)el0_demo, (u64)&el0_stack[4096]);   /* 栈顶 = 专用栈页顶（2026-10-28 起与数据页分离） */
}

/* ---------------- 同步异常处理：转储现场，可恢复的就 ELR+=4 继续跑 ---------------- */
void handle_sync(struct regs *f)
{
    u64 ec = (f->esr >> 26) & 0x3F;

    if (ec == 0x15) {                       /* SVC */
        if ((f->spsr & 0xF) == 0) {         /* 来自 EL0 */
            if (user_running) {             /* 阶段5②：用户程序的系统调用（write/exit） */
                svc_dispatch(f);
                /* 系统调用往返 → 看门狗重新计时（2026-10-28 二查补）：计时起点原本只在
                   enter_el0_at 设一次 → "30 秒无系统调用" 实为 "30 秒总时长"，边算边
                   打印的慢程序会被误杀。exit 分支已 user_running=0，重置对它无影响。 */
                el0_enter_tick = tick;
                return;
            }
            /* 阶段2 的 EL0 演示：SVC = 体面退出用户态 */
            uart_puts("\n[el0] 收到 SVC！EL0 已完成任务\n");
            uart_puts("[el0] EL0 页自测: page[0]="); uart_puthex(el0_page[0]);
            uart_puts(" page[1]="); uart_puthex(el0_page[1]);
            uart_puts(" (期望 0xab / 0xac)\n");
            f->spsr = 0x345;                /* 切回 EL1h（保留 D/A/F 屏蔽，开 IRQ） */
            f->elr  = (u64)el1_landing;     /* eret 直达内核着陆函数 */
        } else {                            /* EL1 自己的 SVC：跳过 */
            f->elr += 4;
        }
        return;
    }

    /* 阶段5②：用户程序跑飞（读内核/写非法地址/取指炸）→ 杀掉回 shell。
       不学 ELR+4 纵容它——那是给内核自愈演示用的，用户代码必须隔离处置 */
    if ((f->spsr & 0xF) == 0 && user_running) {
        uart_puts("\n[run] 用户程序出错(EC=0x"); uart_puthex(ec);
        uart_puts(" FAR="); uart_puthex(f->far); uart_puts(")，杀掉回 shell\n");
        user_running = 0;
        f->spsr = 0x345;
        f->elr  = (u64)el1_landing;
        return;
    }

    uart_puts("\n\n*** 内核异常: ");
    uart_puts(ec_name(ec));
    if (ec == 0x20 || ec == 0x24)          /* 0x20/0x24 = 低 EL 来的取指/数据中止 */
        uart_puts(" [来自 EL0]");
    uart_puts(" ***\n");
    uart_puts("ESR_EL1 = "); uart_puthex(f->esr);
    uart_puts("  (EC=0x");  uart_puthex(ec); uart_puts(")\n");
    uart_puts("FAR_EL1 = "); uart_puthex(f->far);
    uart_puts("   ELR_EL1 = "); uart_puthex(f->elr);
    uart_puts("   SPSR = "); uart_puthex(f->spsr); uart_puts("\n");

    uart_puts("现场寄存器:\n");
    for (int i = 0; i < 30; i += 2) {
        uart_puts("  x"); uart_puthex(i);   uart_puts("="); uart_puthex(f->x[i]);
        uart_puts("  x"); uart_puthex(i+1); uart_puts("="); uart_puthex(f->x[i+1]);
        uart_puts("\n");
    }
    uart_puts("  x30="); uart_puthex(f->x[30]); uart_puts("\n");

    if (ec == 0x00 || ec == 0x24 || ec == 0x25 || ec == 0x3C) {
        f->elr += 4;                      /* 肇事指令都是 4 字节，跳过它 */
        uart_puts("[自愈] ELR+4 跳过肇事指令，继续活着\n> ");
    } else {
        uart_puts("[停机] 无法恢复，CPU 进入 wfe 睡眠\n");
        for (;;)
            __asm__ volatile("wfe");
    }
}

/* FIQ/SError：不该发生，报死后停机 */
void handle_fatal(struct regs *f)
{
    uart_puts("\n*** FATAL: FIQ/SError，无法处理，停机 ***\n");
    uart_puts("ESR="); uart_puthex(f->esr);
    uart_puts(" ELR="); uart_puthex(f->elr); uart_puts("\n");
}

/* ---------------- 演示命令 ---------------- */
static void crash_udf(void)   { __asm__ volatile("udf #0"); }
static void crash_brk(void)   { __asm__ volatile("brk #7"); }
static void crash_abort(void) { (void)*(volatile u32 *)0xdead0000UL; }
static void write_text_page(void)
{
    *(volatile u32 *)0x40080000UL = 0xDEADBEEFU;  /* text 是只读页 → 权限违规 → 转储 + 自愈 */
}

/* ================= 阶段5：用户、口令与权限 ================= */

static int shell_uid = 0;      /* shell 当前身份（0=root）；su 后同步 fs_setuid */

/* 口令参数（盐/迭代数/预计算哈希）统一在 auth_cred.h —— 内核镜像里没有明文口令。
   换口令：改 gen_authhash.c 里的明文 → 跑它 → 把输出哈希粘回 auth_cred.h（见文件头三步）。 */

/* 无 libc 的小串工具：追加字符串/十进制数到缓冲 */
static u32 str_app(char *dst, u32 pos, u32 cap, const char *s)
{
    while (*s && pos + 1 < cap) dst[pos++] = *s++;
    dst[pos] = 0;
    return pos;
}
static u32 str_app_dec(char *dst, u32 pos, u32 cap, u32 v)
{
    char t[12];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && pos + 1 < cap) dst[pos++] = t[--n];   /* t 里是低位在前，倒着写回 */
    dst[pos] = 0;
    return pos;
}

/* 读整个文件进 buf（越界返回 -1），口令/身份解析共用 */
static int read_all(const char *path, u8 *buf, u32 cap)
{
    int ino = fs_lookup(path);
    if (ino < 0) return -1;
    return fs_readfile(ino, buf, cap);
}

/* (ptr,len) 与 C 串全等 */
static int eqn(const char *a, u32 alen, const char *b)
{
    for (u32 i = 0; i < alen; i++)
        if (!b[i] || a[i] != b[i]) return 0;
    return b[alen] == 0;
}
static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 开机建身份三件套（只在 boot 调一次）：
   /etc/passwd 可读 name:uid；/etc/shadow 私密 name:salthex:hashhex:iter（只存哈希绝不存明文）；
   /home/demo 属主改成 demo（1000）——demo 进去才有权建文件 */
static void auth_init(void)
{
    const char *names[2]   = { "root", "demo" };
    const char *uidstr[2]  = { "0",   "1000" };
    const char *hashes[2]  = { PW_ROOT_HASH, PW_DEMO_HASH };
    char buf[256];
    u32 pos;

    /* /etc/passwd */
    int f = fs_createfile("/etc/passwd");
    if (f >= 0) {
        pos = 0;
        for (int i = 0; i < 2; i++) {
            pos = str_app(buf, pos, sizeof(buf), names[i]);
            pos = str_app(buf, pos, sizeof(buf), ":");
            pos = str_app(buf, pos, sizeof(buf), uidstr[i]);
            pos = str_app(buf, pos, sizeof(buf), "\n");
        }
        fs_setfile(f, (const u8 *)buf, pos);
    }

    /* /etc/shadow：每行 name:盐hex:哈希hex:迭代数 */
    f = fs_createfile("/etc/shadow");
    if (f >= 0) {
        pos = 0;
        char shex[17];
        to_hex(AUTH_SALT, 8, shex);            /* 盐是公开参数，随 shadow 一起存 */
        for (int i = 0; i < 2; i++) {
            /* 哈希 = 预计算常量（auth_cred.h，5 万轮迭代 SHA-256）：内核不保存明文、
               开机也不现算；登录时 shadow_verify 拿用户敲的口令现场算再比对 */
            const char *hhex = hashes[i];
            pos = str_app(buf, pos, sizeof(buf), names[i]);
            pos = str_app(buf, pos, sizeof(buf), ":");
            pos = str_app(buf, pos, sizeof(buf), shex);
            pos = str_app(buf, pos, sizeof(buf), ":");
            pos = str_app(buf, pos, sizeof(buf), hhex);
            pos = str_app(buf, pos, sizeof(buf), ":");
            pos = str_app_dec(buf, pos, sizeof(buf), AUTH_ITER);
            pos = str_app(buf, pos, sizeof(buf), "\n");
        }
        fs_setfile(f, (const u8 *)buf, pos);
        fs_chmod(f, FM_PRIVATE);        /* 私密：非 root/demo 读不到（root 属主，root 恒可读） */
    }

    /* /home/demo 属主 demo */
    int h = fs_mkdir("/home/demo");
    if (h >= 0) fs_chown(h, 1000);
}

/* /etc/passwd：按名字查 uid */
static int passwd_lookup(const char *name, int *uid_out)
{
    u8 buf[256];
    int n = read_all("/etc/passwd", buf, sizeof(buf) - 1);
    if (n < 0) return -1;
    buf[n] = 0;
    u32 p = 0;
    while (p < (u32)n) {
        u32 s = p;
        while (p < (u32)n && buf[p] != '\n') p++;
        u32 c1 = s, lend = p;
        for (u32 j = s; j < lend; j++) if (buf[j] == ':') { c1 = j; break; }
        if (c1 > s && eqn((char *)buf + s, c1 - s, name)) {
            int uid = 0;
            for (u32 j = c1 + 1; j < lend; j++) {
                if (buf[j] < '0' || buf[j] > '9') return -1;
                uid = uid * 10 + (buf[j] - '0');
            }
            if (uid_out) *uid_out = uid;
            return 0;
        }
        if (p < (u32)n) p++;
    }
    return -1;
}

/* /etc/passwd：按 uid 反查名字 */
static int passwd_name_of(int uid, char *out, int cap)
{
    u8 buf[256];
    int n = read_all("/etc/passwd", buf, sizeof(buf) - 1);
    if (n < 0) return -1;
    buf[n] = 0;
    u32 p = 0;
    while (p < (u32)n) {
        u32 s = p;
        while (p < (u32)n && buf[p] != '\n') p++;
        u32 c1 = s, lend = p;
        for (u32 j = s; j < lend; j++) if (buf[j] == ':') { c1 = j; break; }
        if (c1 > s) {
            int v = 0;
            for (u32 j = c1 + 1; j < lend; j++) {
                if (buf[j] < '0' || buf[j] > '9') { v = -1; break; }
                v = v * 10 + (buf[j] - '0');
            }
            if (v == uid) {
                u32 len = c1 - s;
                if (len > (u32)cap - 1) len = (u32)cap - 1;
                for (u32 j = 0; j < len; j++) out[j] = buf[s + j];
                out[len] = 0;
                return 0;
            }
        }
        if (p < (u32)n) p++;
    }
    return -1;
}

/* /etc/shadow：校验 name 的口令 pw。su 里临时以 root 读私密文件（同 Unix setuid su） */
static int shadow_verify(const char *name, const char *pw)
{
    u8 buf[320];
    int save = fs_getuid();
    fs_setuid(0);                       /* 特权读 */
    int n = read_all("/etc/shadow", buf, sizeof(buf) - 1);
    fs_setuid(save);
    if (n < 0) return -1;
    buf[n] = 0;
    u32 p = 0;
    while (p < (u32)n) {
        u32 s = p;
        while (p < (u32)n && buf[p] != '\n') p++;
        u32 lend = p;
        /* 切四段：name:salthex:hashhex:iter（找三个冒号） */
        u32 c2 = 0, c3 = 0, c4 = 0;
        int ncol = 0;
        for (u32 j = s; j < lend; j++) {
            if (buf[j] == ':') {
                ncol++;
                if (ncol == 1) c2 = j;
                else if (ncol == 2) c3 = j;
                else if (ncol == 3) { c4 = j; break; }
            }
        }
        if (ncol == 3 && eqn((char *)buf + s, c2 - s, name)) {
            u32 sl = c3 - c2 - 1, hl = c4 - c3 - 1, il = lend - c4 - 1;
            if (sl != 16 || hl != 64 || il == 0 || il > 6) return -1;
            u32 iter = 0;
            for (u32 j = 0; j < il; j++) {
                int d = hexv(buf[c4 + 1 + j]);
                if (d < 0) return -1;
                iter = iter * 10 + (u32)d;
            }
            u8 salt[8];
            for (u32 j = 0; j < 8; j++) {
                int hi = hexv(buf[c2 + 1 + j * 2]), lo = hexv(buf[c2 + 2 + j * 2]);
                if (hi < 0 || lo < 0) return -1;
                salt[j] = (u8)(hi * 16 + lo);
            }
            u8 h[32];
            char got[65];
            sha256_iter(salt, 8, pw, iter, h);
            to_hex(h, 32, got);
            for (u32 j = 0; j < 64; j++)
                if (got[j] != buf[c3 + 1 + j]) return -1;
            return 0;
        }
        if (p < (u32)n) p++;
    }
    return -1;
}

/* 阻塞读一行（su 的口令提示用）。hidden=1 不回显（防"肩窥"）。
   等待期间 wfi 睡觉——抢占调度照常，worker 心跳不受影响。 */
static void blocking_getline(char *buf, u32 cap, int hidden)
{
    u32 n = 0;
    buf[0] = 0;
    for (;;) {
        char c;
        usb_kbd_poll();                  /* 阶段6①：USB 键盘也往同一个 ring 塞 */
        if (!ring_pop(&c)) { __asm__ volatile("wfi"); continue; }
        if (c == '\r' || c == '\n') {
            buf[n] = 0;
            uart_puts("\n");
            return;
        }
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; if (!hidden) uart_puts("\b \b"); }
            continue;
        }
        if (c >= 32 && c < 127 && n + 1 < cap) {
            buf[n++] = c;
            if (!hidden) uart_putc(c);       /* hidden 时什么都不打 */
        }
    }
}

/* ---------------- 行模式 shell（阶段4：带参数的命令 + 退格） ---------------- */
static char line[160];
static int  linelen;

static void cmd_ls(char *a);
static void cmd_cd(char *a);
static void cmd_pwd(char *a);
static void cmd_cat(char *a);
static void cmd_write(char *a);
static void cmd_mkdir(char *a);
static void cmd_rm(char *a);
static void cmd_df(char *a);
static void cmd_whoami(char *a);
static void cmd_su(char *a);
static void cmd_crash(char *a);
static void cmd_mmu(char *a);
static void cmd_tasks(char *a);
static void cmd_el0(char *a);
static void cmd_help(char *a);

struct cmdent { const char *name; void (*fn)(char *); };
/* ================= kernel bench：内核态微基准 =================
 * 用途：底层优化前后用同一把尺子量（us 越小、MB/s 越大越好）。
 * 只测「纯内核路径」：内存拷贝、帧缓冲清屏/滚动/画字；不测串口（UART 是 115200 的瓶颈）。 */
/* ⚠️ 512B×2：内核 .bss 离 USER_BASE 只剩十几 KB（virtio-blk 队列要 8KB），别把镜像顶穿用户区 */
static u8 bench_a[512] __attribute__((aligned(64)));
static u8 bench_b[512] __attribute__((aligned(64)));

static inline u64 bench_cnt(void) { u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline u64 bench_hz(void)  { u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

static void bench_line(const char *name, u64 bytes, u64 cyc, u64 hz)
{
    uart_puts("  "); uart_puts(name); uart_puts(": ");
    u32 us = (u32)(cyc / (hz / 1000000));            /* 24MHz → 每 us 24 ticks */
    uart_putdec(us); uart_puts(" us");
    if (bytes) {
        u64 mbps = cyc ? (bytes * hz / cyc / (1024u * 1024u)) : 0;
        uart_puts(", "); uart_putdec((u32)mbps); uart_puts(" MB/s");
    }
    uart_puts("\n");
}

static void cmd_bench(char *args)
{
    (void)args;
    u64 hz = bench_hz();
    uart_puts("⚠️ 本命令会清屏并滚动屏幕（测的就是这两个操作），屏幕会花，属正常\n");
    uart_puts("内核微基准 (cntfrq="); uart_putdec((u32)hz); uart_puts("Hz)\n");
    uart_puts("  fb="); uart_puthex((u64)fb_mem);
    uart_puts(" w="); uart_putdec(fb_w); uart_puts(" h="); uart_putdec(fb_h);
    uart_puts(" stride="); uart_putdec(fb_stride);
    uart_puts(" u64ok="); uart_putdec((u32)fb_u64_ok); uart_puts("\n");
    u64 t0, t1;

    /* 1) fs_memcpy 1MB（64KB × 16） */
    t0 = bench_cnt();
    for (int i = 0; i < 2048; i++) fs_memcpy(bench_a, bench_b, sizeof bench_a);   /* 2048×512B = 1MB */
    t1 = bench_cnt();
    bench_line("memcpy 1MB", 1024 * 1024, t1 - t0, hz);

    /* 2) 帧缓冲清屏一屏 */
    if (fb_mem) {
        u64 bytes = (u64)fb_w * fb_h * 4;
        t0 = bench_cnt(); fb_clear_all(); t1 = bench_cnt();
        bench_line("fb 清屏 1 屏", bytes, t1 - t0, hz);
    }
    /* 3) 帧缓冲滚动一行 ×8（就是每滚一行要搬多少像素） */
    if (fb_mem) {
        u64 bytes = (u64)fb_w * (fb_h - FONT_H) * 4 * 8;
        t0 = bench_cnt(); for (int i = 0; i < 8; i++) fb_scroll(); t1 = bench_cnt();
        bench_line("fb 滚动 x8", bytes, t1 - t0, hz);
    }
    /* 4) 画 1000 个 ASCII 字形（每轮把光标归零，避免不同位置影响） */
    if (fb_mem) {
        u32 sx = fb_x, sy = fb_y;
        t0 = bench_cnt();
        for (int i = 0; i < 1000; i++) { fb_x = 0; fb_y = 0; fb_draw_cp('A'); }
        t1 = bench_cnt();
        fb_x = sx; fb_y = sy;
        bench_line("fb 画 1000 字符", 1000ull * FONT_W * FONT_H * 4, t1 - t0, hz);
    }
    uart_puts("提示: us 越小越好, MB/s 越大越好\n");
}

static void cmd_disks(char *a)
{
    (void)a;
    if (!vb_present()) { uart_puts("没有 virtio-blk 磁盘\n"); return; }
    u64 mb = vb_capacity_bytes() / (1024 * 1024);
    uart_puts("磁盘 0: "); uart_putdec((u32)mb); uart_puts(" MB, ");
    uart_putdec((u32)vb_capacity_bytes()); uart_puts(" 字节\n");
    u8 sec[512];
    if (vb_read(0, sec, 1) == 0) {
        uart_puts("  扇区0 前 16 字节: ");
        for (int i = 0; i < 16; i++) { uart_puthex(sec[i]); uart_puts(" "); }
        uart_puts("\n  ");
        int has_sig = 0;
        for (int i = 0; i < 512; i++) if (sec[i]) { has_sig = 1; break; }
        uart_puts(has_sig ? "（非空：可能已有分区/系统）[not-empty]\n" : "（空盘）[empty]\n");
        if (sec[510] == 0x55 && sec[511] == 0xAA) uart_puts("  ✓ 有 MBR 签名\n");
    } else {
        uart_puts("  读扇区 0 失败\n");
    }
}

/* ⚠️ 破坏性测试：写盘最后 8 个扇区再读回比对（不碰前面，安全）*/
static void cmd_disktest(char *a)
{
    (void)a;
    if (!vb_present()) { uart_puts("没有磁盘\n"); return; }
    u64 total = vb_capacity_bytes() / 512;
    u8 w[512], r[512];
    for (int i = 0; i < 512; i++) w[i] = (u8)(i * 7 + 0x5A);
    u64 lba = total - 8;
    if (vb_write(lba, w, 1) != 0) { uart_puts("写失败\n"); return; }
    for (int i = 0; i < 512; i++) r[i] = 0;
    if (vb_read(lba, r, 1) != 0) { uart_puts("读失败\n"); return; }
    int bad = 0;
    for (int i = 0; i < 512; i++) if (r[i] != w[i]) bad++;
    uart_puts("[disktest] LBA "); uart_putdec((u32)lba);
    if (bad == 0) uart_puts(" 写读一致 [ok]\n");
    else {
        uart_puts(" 不一致 [bad] 差异字节 "); uart_putdec((u32)bad); uart_puts("\n");
    }
}

/* ---- 阶段6③：把系统装到硬盘 ----
   做法：写 MBR（p1=ESP FAT / p2=LoongOS 数据区）→ 把 stub 读进内存的 esp.img 原样写进 p1
        → 在我们的分区上用本内核自己的文件系统格式化（不需要 Linux、不需要 FAT 写入代码）。 */
static void cmd_install(char *args)
{
    if (!vb_present()) { uart_puts("[install] FAIL no-disk\n"); return; }
    if (!g_bi || g_bi->magic != BOOTINFO_MAGIC || !g_bi->img_addr || !g_bi->img_size) {
        uart_puts("[install] FAIL no-esp-payload\n");
        return;
    }
    uart_puts("[install] START\n");
    if (!args || fs_strcmp(args, "yes") != 0) {
        uart_puts("⚠️ 安装会清空整块磁盘！确认请敲: install yes\n");
        return;
    }
    u64 total = vb_capacity_sectors();
    u64 img_sec = (g_bi->img_size + 511) / 512;
    u64 esp_lba = 2048;
    u64 p2_lba = esp_lba + ((img_sec + 2047) / 2048) * 2048;   /* 1MB 对齐 */
    if (p2_lba + 8192 > total) {              /* 8192 扇区 = 数据区下限 4MB，文案和判据对齐 */
        uart_puts("磁盘太小：ESP（1MB 偏移 + 镜像按 1MB 对齐）之后还要 ≥4MB 数据区，当前全盘 ");
        uart_putdec((u32)(total / 2048)); uart_puts(" MB\n");
        return;
    }
    u64 p2_sec = total - p2_lba;

    /* 1. MBR */
    static u8 mbr[512];
    fs_memset(mbr, 0, sizeof(mbr));
    mbr[446 + 0] = 0x80;                 /* 可启动标志 */
    mbr[446 + 4] = 0xEF;                 /* ESP 分区类型（UEFI 规范值；旧 0x0C=FAT32-LBA 在 QEMU 也认，但不规范） */
    mbr[446 + 8]  = (u8)esp_lba;  mbr[446 + 9]  = (u8)(esp_lba >> 8);
    mbr[446 + 10] = (u8)(esp_lba >> 16); mbr[446 + 11] = (u8)(esp_lba >> 24);
    mbr[446 + 12] = (u8)img_sec; mbr[446 + 13] = (u8)(img_sec >> 8);
    mbr[446 + 14] = (u8)(img_sec >> 16); mbr[446 + 15] = (u8)(img_sec >> 24);
    mbr[462 + 0] = 0x00;
    mbr[462 + 4] = 0x99;                 /* LoongOS 数据分区 */
    mbr[462 + 8]  = (u8)p2_lba;  mbr[462 + 9]  = (u8)(p2_lba >> 8);
    mbr[462 + 10] = (u8)(p2_lba >> 16); mbr[462 + 11] = (u8)(p2_lba >> 24);
    mbr[462 + 12] = (u8)p2_sec; mbr[462 + 13] = (u8)(p2_sec >> 8);
    mbr[462 + 14] = (u8)(p2_sec >> 16); mbr[462 + 15] = (u8)(p2_sec >> 24);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (vb_write(0, mbr, 1) != 0) { uart_puts("[install] FAIL mbr\n"); return; }
    uart_puts("[install] MBR-OK\n");

    /* 2. ESP：esp.img 原样拷进去（分 4KB 段，尾部补齐） */
    uart_puts("[install] 写 ESP ");
    uart_putdec((u32)(g_bi->img_size / 1024)); uart_puts(" KB ...\n");
    for (u64 off = 0; off < g_bi->img_size; off += 4096) {
        u64 n = g_bi->img_size - off;
        if (n > 4096) n = 4096;
        u64 sec = (n + 511) / 512;
        if (vb_write(esp_lba + off / 512, (u8 *)g_bi->img_addr + off, (u32)sec) != 0) {
            uart_puts("\n[install] FAIL esp write off="); uart_putdec((u32)off); uart_puts("\n");
            return;
        }
    }

    /* 3. 数据分区：用本内核的文件系统格式化 */
    u32 nblk = (u32)(p2_sec / 8);
    uart_puts("[install] 格式化数据分区 块数="); uart_putdec(nblk); uart_puts(" ...\n");
    uart_puts("[install] ESP-OK\n");
    if (fs_format_disk(p2_lba, nblk) != 0) { uart_puts("[install] FAIL format\n"); return; }
    uart_puts("[install] FMT-OK\n");

    uart_puts("✅ 安装完成！关机后移除 ISO（光盘），从硬盘启动即可。\n");
    uart_puts("[install] DONE\n");   /* 纯 ASCII 完成标记（测试/脚本认这行） */
}

static void cmd_scsi(char *a)
{
    (void)a;
    {
        extern int vs_dbg_bdf; extern u32 vs_dbg_id; extern u32 vs_dbg_raw[6];
        uart_puts("[scsi] dbg bdf="); uart_putdec((u32)(vs_dbg_bdf < 0 ? 0 : vs_dbg_bdf));
        uart_puts(" id="); uart_puthex(vs_dbg_id);
        uart_puts(" rawBAR:");
        for (int i = 0; i < 6; i++) { uart_puts(" "); uart_puthex(vs_dbg_raw[i]); }
        uart_puts("\n");
    }
    int r = vs_init();
    uart_puts("[scsi] reinit rc="); uart_putdec((u32)(r < 0 ? -r : r));
    uart_puts(" present="); uart_putdec((u32)vs_present());
    if (vs_present()) { uart_puts(" MB="); uart_putdec((u32)(vs_capacity_bytes() / (1024 * 1024))); }
    uart_puts("\n");
}

/* ---- 阶段7：联网命令 ---- */
/* 把 p 里的下一个词摘出来（原地截断，返回词尾下一个位置；没有词了返回 0） */
static char *next_word(char **pp)
{
    char *p = *pp;
    while (*p == ' ') p++;
    if (!*p) { *pp = p; return 0; }
    char *w = p;
    while (*p && *p != ' ') p++;
    if (*p) { *p = 0; p++; }
    *pp = p;
    return w;
}

/* net                 初始化网卡驱动（不配 IP）
   net dhcp            走 DHCP 拿 IP
   net static <ip> [掩码] [网关]   手配 */
static void cmd_net(char *args)
{
    char *a = args;
    char *sub = next_word(&a);
    if (!sub) { net_init_driver(); return; }
    net_init_driver();
    if (fs_strcmp(sub, "dhcp") == 0) { net_dhcp(); return; }
    if (fs_strcmp(sub, "static") == 0) {
        char *ip  = next_word(&a);
        char *mks = next_word(&a);
        char *gw  = next_word(&a);
        if (!ip) { uart_puts("用法: net static <ip> [掩码] [网关]\n"); return; }
        net_static_config(ip, mks, gw);
        return;
    }
    uart_puts("用法: net | net dhcp | net static <ip> [掩码] [网关]\n");
}

static void cmd_ifconfig(char *args) { (void)args; net_ifconfig_print(); }

/* cpu —— 看多核状态：在线标记 + 计数增量（增量>0 说明那颗核真的在跑） */
static void cmd_cpu(char *args)
{
    (void)args;
    extern volatile unsigned long smp_pcpu_tick(int cpu);
    uart_puts("本核 MPIDR.aff0="); uart_putdec((u32)smp_cpu_id());
    uart_puts("  本核 tick="); uart_putdec((u32)smp_pcpu_tick(0)); uart_puts("\n");
    unsigned long c0[4];
    for (int i = 0; i < 4; i++) c0[i] = smp_pcpu_tick(i);
    for (volatile int k = 0; k < 2000000; k++) { }      /* 粗延时，让从核把计数推上去 */
    for (int i = 0; i < 4; i++) {
        uart_puts("  cpu"); uart_putdec((u32)i);
        uart_puts(smp_online[i] ? "  在线  " : "  未启动");
        uart_puts("  tick "); uart_putdec((u32)smp_pcpu_tick(i));
        uart_puts("  (+"); uart_putdec((u32)(smp_pcpu_tick(i) - c0[i])); uart_puts(")\n");
    }
}

/* tcp get <ip> <端口> —— 连上去读最多 3 秒（80 端口自动发一个 GET，其余当纯读，比如 22 端口读 SSH 横幅） */
static void cmd_tcp(char *args)
{
    char *a = args;
    char *sub = next_word(&a);
    if (!sub || fs_strcmp(sub, "get") != 0) {
        uart_puts("用法: tcp get <ip> <端口>\n");
        return;
    }
    char *ips = next_word(&a);
    char *ps  = next_word(&a);
    if (!ips || !ps) { uart_puts("用法: tcp get <ip> <端口>\n"); return; }
    u32 ip = 0;
    {   /* 复用 net.c 的解析：这里简单自己算 */
        int part = 0, val = 0, dig = 0; u32 v[4] = {0,0,0,0};
        for (const char *p = ips;; p++) {
            if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); dig++; }
            else if (*p == '.' || *p == 0) {
                if (!dig || part >= 4) { uart_puts("IP 格式不对\n"); return; }
                v[part++] = (u32)val; val = 0; dig = 0;
                if (!*p) break;
            } else { uart_puts("IP 格式不对\n"); return; }
        }
        if (part != 4) { uart_puts("IP 格式不对\n"); return; }
        ip = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    }
    int port = 0;
    for (const char *p = ps; *p; p++) { if (*p < '0' || *p > '9') { uart_puts("端口不对\n"); return; } port = port * 10 + (*p - '0'); }
    if (port <= 0 || port > 65535) { uart_puts("端口不对\n"); return; }

    uart_puts("[tcp] 连接 "); uart_puts(ips); uart_puts(":"); uart_putdec((u32)port); uart_puts(" …\n");
    int r = tcp_connect(ip, (unsigned short)port, 5000);
    if (r != 0) { uart_puts("[tcp] 连不上 r="); uart_putdec((u32)(-r)); uart_puts("\n"); return; }
    uart_puts("[tcp] 已建立 ✅\n");
    if (port == 80) {
        const char *req = "GET / HTTP/1.0\r\nHost: loongos\r\n\r\n";
        int n = 0; while (req[n]) n++;
        tcp_send(req, n);
    }
    static char rb[512];
    int total = 0;
    for (int i = 0; i < 6; i++) {          /* 最多 3 秒，每次 500ms */
        int n = tcp_recv(rb, sizeof rb - 1, 500);
        if (n < 0) break;
        if (n == 0) continue;
        rb[n] = 0;
        total += n;
        for (int k = 0; k < n; k++) { if (rb[k] == '\r') continue; uart_putc(rb[k]); }
    }
    uart_puts("\n[tcp] 共收到 "); uart_putdec((u32)total); uart_puts(" 字节\n");
    tcp_close();
}

/* ping <ip> [次数] */
static void cmd_ping(char *args)
{
    char *a = args;
    char *ip = next_word(&a);
    char *cs = next_word(&a);
    if (!ip) { uart_puts("用法: ping <ip> [1-10]\n"); return; }
    int cnt = 0;
    if (cs) { cnt = 0; for (const char *p = cs; *p; p++) { if (*p < '0' || *p > '9') { cnt = 0; goto cnt_done; } cnt = cnt * 10 + (*p - '0'); } }
cnt_done:
    net_ping(ip, cnt);
}

static const struct cmdent CMDS[] = {
    { "ls",    cmd_ls    }, { "cat",   cmd_cat   }, { "write", cmd_write },
    { "mkdir", cmd_mkdir }, { "rm",    cmd_rm    }, { "df",    cmd_df    },
    { "whoami",cmd_whoami}, { "su",    cmd_su    }, { "run",   cmd_run   },
    { "cd",    cmd_cd    }, { "pwd",  cmd_pwd   },
    { "crash", cmd_crash }, { "mmu",   cmd_mmu   }, { "tasks", cmd_tasks },
    { "el0",   cmd_el0   }, { "help",  cmd_help  }, { "bench", cmd_bench },
    { "disks", cmd_disks }, { "disktest", cmd_disktest }, { "install", cmd_install }, { "scsi", cmd_scsi },
    { "net", cmd_net }, { "ifconfig", cmd_ifconfig }, { "ping", cmd_ping }, { "tcp", cmd_tcp }, { "cpu", cmd_cpu },
    { 0, 0 }
};

/* 阶段5④：把 "cmd args > 出文件 < 入文件" 拆开。
   规则：> 和 < 必须是独立的词、各只能出现一次、后面必须紧跟路径；
   拆出的命令部分在 cmdout（词间单空格），未出现的方向对应缓冲为 ""。 */
static int redir_parse(const char *line, char *cmdout, int ccap,
                       char *outp, int ocap, char *inp, int icap)
{
    int o = 0, saw_out = 0, saw_in = 0;
    cmdout[0] = outp[0] = inp[0] = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - s);
        if (len == 1 && (*s == '>' || *s == '<')) {      /* 重定向符号：吃掉后面的目标词 */
            char dir = *s;                               /* ⚠️ 先记住方向，s 马上要改指目标词 */
            if (dir == '>') { if (saw_out) return -1; saw_out = 1; }
            else            { if (saw_in)  return -1; saw_in  = 1; }
            while (*p == ' ') p++;
            if (!*p) return -1;                          /* "> 后面没东西 */
            s = p;
            while (*p && *p != ' ') p++;
            len = (int)(p - s);
            char  *dst = (dir == '>') ? outp : inp;
            int    cap = (dir == '>') ? ocap : icap;
            if (len >= cap) return -1;
            fs_memcpy(dst, s, (u32)len);
            dst[len] = 0;
            continue;
        }
        if (o + len + 2 >= ccap) return -1;              /* 命令部分 */
        if (o) cmdout[o++] = ' ';
        fs_memcpy(cmdout + o, s, (u32)len);
        o += len;
        cmdout[o] = 0;
    }
    return 0;
}

/* 输出重定向收尾：命令跑完把缓冲落盘（覆盖写），并到串口报告结果 */
static void redir_finish(void)
{
    redir_on = 0;
    if (redir_len < 0) return;
    int n = redir_len;
    redir_len = -1;
    int ino = fs_lookup(redir_path);
    if (ino < 0) ino = fs_createfile(redir_path);
    if (ino == FS_DENIED) {
        uart_puts("权限不够：写不了 "); uart_puts(redir_path); uart_puts("\n");
    } else if (ino < 0) {
        uart_puts("建不了文件（父目录缺？）: "); uart_puts(redir_path); uart_puts("\n");
    } else if (fs_setfile(ino, (const u8 *)redir_buf, (u32)n) < 0) {
        uart_puts("写文件失败（盘满？）: "); uart_puts(redir_path); uart_puts("\n");
    } else {
        uart_puts("已重定向 "); uart_putdec((u32)n);
        uart_puts(" 字节 -> "); uart_puts(redir_path); uart_puts("\n");
        if (redir_of) uart_puts("（输出超 2048 字节，已截断）\n");
    }
    redir_of = 0;
}

static void dispatch(char *s)
{
    char cmd[160], outp[128], inp[128];
    while (*s == ' ') s++;
    if (!*s) return;
    if (redir_parse(s, cmd, sizeof cmd, outp, sizeof outp, inp, sizeof inp) < 0) {
        uart_puts("重定向语法错：> / < 后面要紧跟路径，且各只能出现一次\n");
        return;
    }
    if (!*cmd) return;

    /* 输入重定向：先把 < 文件整份读进 stdin_buf（目前只有 write 消费它） */
    redir_in_len = -1;
    if (inp[0]) {
        char rp[128];
        if (path_resolve(inp, rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
        int ino = fs_lookup(rp);
        if (ino < 0)  { uart_puts("没有这个文件: "); uart_puts(rp); uart_puts("\n"); return; }
        int n = fs_readfile(ino, (u8 *)stdin_buf, sizeof stdin_buf);
        if (n < 0)   { uart_puts("读不了（是目录？私密？）: "); uart_puts(rp); uart_puts("\n"); return; }
        if (n == (int)sizeof stdin_buf) { uart_puts("输入文件太大（上限 2047 字节）\n"); return; }
        redir_in_len = n;
    }

    /* 拆命令词和参数（重定向词已被剥掉） */
    char *cmdw = cmd;
    char *args = 0;
    char *p = cmd;
    while (*p && *p != ' ') p++;
    if (*p) {
        *p++ = 0;
        while (*p == ' ') p++;
        if (*p) args = p;
    }

    /* ⚠️ 输入支持检查必须在拆词之后、拿 cmdw（首词）比 —— 拿整行比 "write" 永远不等 */
    if (redir_in_len >= 0 && fs_strcmp(cmdw, "write") != 0) {
        uart_puts("『"); uart_puts(cmdw);
        uart_puts("』不支持输入重定向（< 目前只有 write 能吃：write 目标 < 源）\n");
        redir_in_len = -1;
        return;
    }

    /* 输出重定向：布置好总闸再跑命令，跑完落盘 */
    if (outp[0]) {
        if (path_resolve(outp, redir_path, sizeof redir_path) < 0) { uart_puts("路径太长\n"); return; }
        redir_len = 0;
        redir_of  = 0;
        redir_on  = 1;
    }
    for (const struct cmdent *k = CMDS; k->name; k++) {
        if (fs_strcmp(cmdw, k->name) == 0) {
            k->fn(args ? args : "");   /* 无参数命令给空串，别让命令里解引用 NULL */
            redir_finish();
            return;
        }
    }
    redir_finish();                       /* 先收闸：未知命令的抱怨走串口，不进文件（相当于 stderr） */
    uart_puts("未知命令: "); uart_puts(cmdw); uart_puts("（help 看清单）\n");
}

static void on_char(char c)
{
    if (c == '\r' || c == '\n') {
        uart_puts("\n");
        line[linelen] = 0;
        linelen = 0;
        dispatch(line);
        uart_puts("> ");
        return;
    }
    if (c == '\b' || c == 127) {
        if (linelen > 0) { linelen--; uart_puts("\b \b"); }
        return;
    }
    if (c >= 32 && c < 127 && linelen < (int)sizeof(line) - 1) {
        line[linelen++] = c;
        uart_putc(c);
    }
}

/* —— fs 类命令 —— */
static void ls_cb(const struct dirent *de, int ino, void *arg)
{
    (void)arg;
    int t = fs_filetype(ino);
    uart_puts(t == FS_T_DIR ? "d  " : "f  ");
    uart_puts(de->name);
    if (t == FS_T_FILE) {
        uart_puts("  ");
        uart_putdec((u32)fs_filesize(ino));
        uart_puts("B");
    }
    uart_puts("\n");
}
/* ---------------- 阶段5③：cwd + 相对路径（全在 shell 侧，fs.c 零改动） ---------------- */
static char cwd[128] = "/";          /* 当前目录（规范绝对路径）；开机在根 */

/* 把输入路径解析成规范绝对路径：绝对路径直接用，相对路径接在 cwd 后；
   逐段处理 "."（本层）".."（上跳，到根为止）"//"（忽略）；太长返回 -1 */
static int path_resolve(const char *in, char *out, int cap)
{
    char tmp[288];
    int n;
    if (in && *in == '/') {
        n = (int)fs_strlen(in);
        if (n >= (int)sizeof tmp) return -1;
        fs_memcpy(tmp, in, (u32)n + 1);
    } else {
        int cl = (int)fs_strlen(cwd);
        int il = in ? (int)fs_strlen(in) : 0;
        if (cl + 1 + il >= (int)sizeof tmp) return -1;
        fs_memcpy(tmp, cwd, (u32)cl);
        tmp[cl] = '/';
        fs_memcpy(tmp + cl + 1, in ? in : "", (u32)il + 1);
    }
    int o = 0;
    out[o++] = '/';
    for (int i = 0; tmp[i]; ) {
        int s = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - s;
        while (tmp[i] == '/') i++;
        if (len == 0 || (len == 1 && tmp[s] == '.'))
            continue;                                  /* 空段 / "."：跳过 */
        if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
            if (o > 1) {                               /* 根上再 ".." 也留在根 */
                int e = o - 1;
                while (e > 0 && out[e] != '/') e--;
                o = (e == 0) ? 1 : e;                  /* 弹掉最后一段 */
            }
            continue;
        }
        if (o > 1) {                                   /* 根的 '/' 已在，其余段补 '/' */
            if (o + 1 >= cap) return -1;
            out[o++] = '/';
        }
        if (o + len >= cap) return -1;
        fs_memcpy(out + o, tmp + s, (u32)len);
        o += len;
    }
    out[o] = 0;
    return 0;
}

static void cmd_cd(char *a)
{
    char rp[128];
    if (path_resolve((a && *a) ? a : "/", rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
    int ino = fs_lookup(rp);
    if (ino < 0)  { uart_puts("没有这个目录: "); uart_puts(rp); uart_puts("\n"); return; }
    if (!fs_isdir(ino)) { uart_puts("不是目录: "); uart_puts(rp); uart_puts("\n"); return; }
    fs_memcpy(cwd, rp, (u32)fs_strlen(rp) + 1);
}
static void cmd_pwd(char *a) { (void)a; uart_puts(cwd); uart_puts("\n"); }

static void cmd_ls(char *a)
{
    char rp[128];
    const char *path;
    if (a && *a) {
        if (path_resolve(a, rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
        path = rp;
    } else {
        path = cwd;                       /* 无参数 = 当前目录 */
    }
    int ino = fs_lookup(path);
    if (ino < 0) { uart_puts("没有这个路径: "); uart_puts(path); uart_puts("\n"); return; }
    if (!fs_isdir(ino)) { uart_puts("f  "); uart_puts(path); uart_puts("  "); uart_putdec((u32)fs_filesize(ino)); uart_puts("B\n"); return; }
    fs_list(ino, ls_cb, 0);
}
static void cmd_cat(char *a)
{
    char rp[128];
    if (!a || !*a) { uart_puts("用法: cat <路径>\n"); return; }
    if (path_resolve(a, rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
    int ino = fs_lookup(rp);
    if (ino < 0) { uart_puts("没有这个路径: "); uart_puts(rp); uart_puts("\n"); return; }
    if (fs_isdir(ino)) { uart_puts(a); uart_puts(" 是目录，不能 cat\n"); return; }
    u8 buf[1024];
    int n = fs_readfile(ino, buf, sizeof(buf) - 1);
    if (n == FS_DENIED) { uart_puts("权限不够：私密文件，只有属主和 root 能看\n"); return; }
    if (n < 0) { uart_puts("读失败\n"); return; }
    for (int i = 0; i < n; i++) uart_putc((char)buf[i]);
    if (n == 0 || buf[n - 1] != '\n') uart_puts("\n");
}
static void cmd_write(char *a)
{
    /* 阶段5④：write 目标 < 源 —— 整份吃 stdin_buf（按字节，可含换行），兼当文件复制 */
    if (redir_in_len >= 0) {
        if (!a || !*a) { uart_puts("用法: write 文件 < 源文件\n"); return; }
        char rp2[128];
        if (path_resolve(a, rp2, sizeof rp2) < 0) { uart_puts("路径太长\n"); return; }
        int ino = fs_lookup(rp2);
        if (ino < 0) ino = fs_createfile(rp2);
        if (ino == FS_DENIED) { uart_puts("权限不够：父目录不是你的: "); uart_puts(rp2); uart_puts("\n"); return; }
        if (ino < 0) { uart_puts("建不了文件（父目录缺？）: "); uart_puts(rp2); uart_puts("\n"); return; }
        int w = fs_setfile(ino, (const u8 *)stdin_buf, (u32)redir_in_len);
        if (w == FS_DENIED) { uart_puts("权限不够：这个文件不是你的，root 才能改\n"); return; }
        if (w < 0) { uart_puts("写失败（盘满？）\n"); return; }
        uart_puts("已写入 "); uart_putdec((u32)redir_in_len);
        uart_puts(" 字节 -> "); uart_puts(rp2); uart_puts("（来自 < 文件）\n");
        return;
    }
    if (!a) { uart_puts("用法: write <路径> <内容>\n"); return; }
    char *path = a;
    while (*a && *a != ' ') a++;
    char *text = 0;
    if (*a) {
        *a++ = 0;
        while (*a == ' ') a++;
        text = a;                     /* 内容 = 剩下整段（可为空串） */
    }
    if (!text) text = (char *)"";
    char rp[128];                              /* 阶段5③：路径走 cwd 解析 */
    if (path_resolve(path, rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
    path = rp;
    int ino = fs_lookup(path);
    if (ino < 0) ino = fs_createfile(path);
    if (ino == FS_DENIED) { uart_puts("权限不够：父目录不是你的（root 才能在这里建文件）: "); uart_puts(path); uart_puts("\n"); return; }
    if (ino < 0) { uart_puts("建不了文件（父目录不存在？盘满？）: "); uart_puts(path); uart_puts("\n"); return; }
    int w = fs_setfile(ino, (const u8 *)text, fs_strlen(text));
    if (w == FS_DENIED) { uart_puts("权限不够：这个文件不是你的，root 才能改\n"); return; }
    if (w < 0) { uart_puts("写失败（盘满？）\n"); return; }
    uart_puts("已写入 "); uart_putdec(fs_strlen(text)); uart_puts(" 字节 -> "); uart_puts(path); uart_puts("\n");
}
static void cmd_mkdir(char *a)
{
    char rp[128];
    if (!a || !*a) { uart_puts("用法: mkdir <路径>\n"); return; }
    if (path_resolve(a, rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
    int r = fs_mkdir(rp);
    if (r == FS_DENIED) { uart_puts("权限不够：父目录不是你的（root 才能在这里建目录）: "); uart_puts(rp); uart_puts("\n"); return; }
    if (r < 0) { uart_puts("建不了目录（已存在？父目录缺？）: "); uart_puts(rp); uart_puts("\n"); return; }
    uart_puts("已建目录 "); uart_puts(rp); uart_puts("\n");
}
static void cmd_rm(char *a)
{
    char rp[128];
    if (!a || !*a) { uart_puts("用法: rm <路径>\n"); return; }
    if (path_resolve(a, rp, sizeof rp) < 0) { uart_puts("路径太长\n"); return; }
    int r = fs_unlink(rp);
    if (r == FS_DENIED) { uart_puts("权限不够：这个文件不是你的，root 才能删: "); uart_puts(rp); uart_puts("\n"); return; }
    if (r < 0) { uart_puts("删不了（不存在？目录非空？）: "); uart_puts(rp); uart_puts("\n"); return; }
    uart_puts("已删除 "); uart_puts(rp); uart_puts("\n");
}
static void cmd_df(char *a)
{
    (void)a;
    u32 used = 0, total = 0;
    fs_statfs(&used, &total);
    uart_puts(fs_is_disk() ? "磁盘: 已用 " : "RAM 盘: 已用 ");   /* 按介质说实话（原来恒写 RAM 盘，磁盘模式下是错的） */
    uart_putdec(used); uart_puts(" / ");
    uart_putdec(total); uart_puts(" 块（");
    uart_putdec(used * 4); uart_puts(" KB / ");
    uart_putdec(total * 4); uart_puts(" KB）\n");
}

/* —— 阶段5：身份命令 —— */
static void cmd_whoami(char *a)
{
    (void)a;
    char nm[24];
    if (passwd_name_of(shell_uid, nm, sizeof(nm)) < 0) {
        uart_puts("uid="); uart_putdec((u32)shell_uid); uart_puts("（不在 /etc/passwd 里）\n");
        return;
    }
    uart_puts(nm); uart_puts(" (uid="); uart_putdec((u32)shell_uid); uart_puts(")\n");
}

static void cmd_su(char *a)
{
    const char *target = (a && *a) ? a : "root";
    int tuid = -1;
    if (passwd_lookup(target, &tuid) < 0) {
        uart_puts("没有这个用户: "); uart_puts(target); uart_puts("\n");
        return;
    }
    if (tuid == shell_uid) {
        uart_puts("已经是 "); uart_puts(target); uart_puts(" 了\n");
        return;
    }
    if (shell_uid != 0) {
        /* 非 root 切换要口令；root 切谁都免（教学简化） */
        uart_puts("口令:");
        char pw[64];
        blocking_getline(pw, sizeof(pw), 1);   /* 不回显 */
        if (shadow_verify(target, pw) != 0) {
            uart_puts("口令不对\n");
            return;
        }
    }
    shell_uid = tuid;
    fs_setuid(tuid);                           /* 文件系统侧同步换身份 */
    uart_puts("现在是 "); uart_puts(target);
    uart_puts("（uid="); uart_putdec((u32)tuid); uart_puts("），文件按这个身份管\n");
}

/* —— 老功能收编成命令 —— */
static void cmd_crash(char *a)
{
    if (a && fs_strcmp(a, "udf") == 0)       { crash_udf();     return; }
    if (a && fs_strcmp(a, "brk") == 0)       { crash_brk();     return; }
    if (a && fs_strcmp(a, "abort") == 0)     { crash_abort();   return; }
    if (a && fs_strcmp(a, "wronly") == 0)    { write_text_page(); return; }
    uart_puts("用法: crash udf|brk|abort|wronly（wronly=写只读页演示）\n");
}
static void cmd_mmu(char *a)    { (void)a; mmu_status(); }
static void cmd_tasks(char *a)  { (void)a; show_tasks(); }
static void cmd_el0(char *a)    { (void)a; enter_el0(); }
static void cmd_help(char *a)
{
    (void)a;
    uart_puts(
        "命令清单:\n"
        "  ls [路径]            列目录（无路径=当前目录）\n"
        "  cd [路径]            切换当前目录（默认 /；支持 . .. 和相对路径）\n"
        "  pwd                  看当前目录\n"
        "  cat <路径>           看文件\n"
        "  write <路径> <内容>  建/覆写文件\n"
        "  mkdir <路径>         建目录\n"
        "  rm <路径>            删文件或空目录\n"
        "  df                   内存盘用量\n"
        "  whoami               当前身份\n"
        "  su [用户]            切换用户（要口令；root 免）——默认 root/demo\n"
        "  run <路径> [参数…]   跑用户程序（如 run /bin/cat、run /bin/args hello world）\n"
        "  重定向: cmd > 文件   把命令输出写进文件（如 ls /etc > t.txt）\n"
        "           write 目标 < 源  把源文件整份写给目标（= 文件复制）\n"
        "  mmu | tasks | el0    MMU 状态 / 任务表 / 进 EL0 演示\n"
        "  crash udf|brk|abort|wronly   崩溃演示（都会转储+自愈）\n"
        "  bench                内核微基准（内存/帧缓冲性能）\n"
        "  disks | disktest     看 virtio 磁盘 / 读写自测（写盘尾 8 扇区）\n"
        "  install yes          把系统装到硬盘（会清空整块盘！需从 ISO 启动）\n"
        "  net | net dhcp       初始化网卡 / 走 DHCP 拿 IP\n"
        "  net static <ip> [掩码] [网关]   手配 IP\n"
        "  ifconfig             看网卡状态 / IP / 统计 / ARP 缓存\n"
        "  ping <ip> [1-10]     ICMP ping（带 RTT）\n"
        "  tcp get <ip> <端口>  建 TCP 连接并读最多 3 秒（80 自动发 GET）\n"
        "  cpu                  看多核：哪几颗在线 / 计数是否在涨\n"
        "  help                 本清单\n");
}

static void shell_loop(void)
{
    for (;;) {
        { extern void smp_tick_inc(void); smp_tick_inc(); }   /* 阶段9：CPU0 自己的 per-CPU tick */
        usb_kbd_poll();              /* 阶段6②：先收 USB 键盘事件，再睡 */
        net_poll();                  /* 阶段7：顺手收网络包（ping 应答/DHCP 都在这处理） */
        __asm__ volatile("wfi");     /* 睡到下一个中断来 */
        char c;
        while (ring_pop(&c))
            on_char(c);
    }
}

/* eret 回内核的着陆点：EL0 的 SVC 把我们送到这，重新接管 shell */
static void __attribute__((noinline)) el1_landing(void)
{
    /* 阶段5④：run 是"一去不复返"的命令（exit 后 eret 直达这里、抛弃 dispatch 栈帧），
       输出重定向的收闸必须在着陆点补做——否则闸门一直开着，把后续提示符和下一条命令的
       输出全吸进文件，直到某次 dispatch 末尾才被顺手冲刷（实测 df 的输出就这么丢的）。
       redir_finish 幂等：没开闸（redir_len<0）时直接返回。fault 路径也走这里，一并覆盖。 */
    redir_finish();
    uart_puts("[el1] 已从 EL0 撤回内核，shell 继续\n> ");
    shell_loop();
}

void kernel_main(unsigned long bootinfo)
{
    g_bi = (struct bootinfo *)bootinfo;   /* stub 通过 x0 传进来 */
    fb_init();                            /* 有 GOP 就把字也画到屏幕上 */
    uart_puts("[boot] bi="); uart_puthex((u64)bootinfo);
    if (g_bi) {
        uart_puts(" magic="); uart_puthex(g_bi->magic);
        uart_puts(" base="); uart_puthex(g_bi->base);
        uart_puts(" img="); uart_puthex(g_bi->img_addr);
        uart_puts(" isz="); uart_puthex(g_bi->img_size);
    }
    uart_puts("\n");
    uart_puts("\n========================================\n");
    uart_puts("  灵珑系统 | LoongOS stage5 - 用户与权限\n");
    uart_puts("========================================\n");

    gic_init();
    uart_puts("[gic ] GICv"); uart_putdec(gic_ver); uart_puts(" + PL011 RX 中断已开\n");

    mmu_init();
    uart_puts("[mmu ] MMU 已开启：内核 text 只读、data 可写、设备 Device 映射\n");

    /* 阶段9：拉起从核（只跑计数循环，不碰 fs/UART，避免竞态） */
    { extern void smp_pcpu_setup(unsigned long); smp_pcpu_setup(0); }
    smp_set_gic_ver((int)gic_ver);
    smp_online[0] = 1;
    {
        unsigned long mask = 0;
        /* ⚠️ 2026-10-31：从核 GIC/定时器中断还没打通（IRQ 挂起但进不了处理程序 →
           wfi 立刻返回 → 从核 100% 空转）。在修好之前默认不启动从核，
           免得白白烧掉一个核心。要复现问题就把下面这行的 0 改成 1。 */
        int n = 0;
        if (0) n = smp_start(&mask);
        uart_puts("[smp ] PSCI 拉起从核 "); uart_putdec((u32)n); uart_puts(" 个");
        if (n > 0) {
            uart_puts("（");
            int first = 1;
            for (int i = 1; i < 4; i++) if (mask & (1ul << i)) {
                if (!first) uart_puts(",");
                uart_puts("cpu"); uart_putdec((u32)i); first = 0;
            }
            uart_puts("）");
            int ok = 0;
            for (int i = 1; i < 4; i++) if (mask & (1ul << i)) ok += smp_wait_online((unsigned long)i, 200000);
            uart_puts("，在线 "); uart_putdec((u32)ok); uart_puts(" 个");
        }
        uart_puts("\n");
    }

    /* 中文自检：把 .rodata 里两个汉字按字节打到串口（纯 ASCII hex）。
       若下游把 ≥0x80 的字节降级成 '?'，这行的 hex 仍能证明内核侧数据是否正确：
       "灵珑" = e7 81 b5 e7 8f 91；这里出现别的值 = 内核内存/字符串真的坏了。 */
    {
        const char *chk = "灵珑";
        uart_puts("[strchk] zh-bytes=");
        for (int i = 0; chk[i]; i++) {
            if (i) uart_putc(' ');
            uart_puthex2((u8)chk[i]);
        }
        uart_puts("\n");
    }

    /* ---- 阶段6③：先看有没有装好的磁盘系统，有就挂它，没有就用内存盘 ---- */
    int fs_ready = 0;
    int disk_ok = (vb_init() == 0);
    if (disk_ok) {
        uart_puts("[disk] virtio-blk 就绪，容量 ");
    } else {
        int rs = vs_init();
        if (rs == 0) {
            disk_ok = 1;                   /* 虚拟机把盘挂在 scsi 总线上 → 用 virtio-scsi */
            uart_puts("[disk] virtio-scsi 就绪，容量 ");
        } else {
            uart_puts("[disk] virtio-scsi 初始化失败(r=");
            uart_putdec((u32)(-rs)); uart_puts(")\n");
        }
    }
    if (disk_ok) {
        uart_putdec((u32)(vb_capacity_bytes() / (1024 * 1024)));
        uart_puts(" MB\n");
        static u8 mbr[512];
        if (vb_read(0, mbr, 1) == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            for (int i = 0; i < 4; i++) {
                u8 *e = mbr + 446 + i * 16;
                u32 lba = (u32)e[8] | ((u32)e[9] << 8) | ((u32)e[10] << 16) | ((u32)e[11] << 24);
                u32 cnt = (u32)e[12] | ((u32)e[13] << 8) | ((u32)e[14] << 16) | ((u32)e[15] << 24);
                if (e[4] != 0x99 || !lba || !cnt) continue;      /* 0x99 = LoongOS 数据分区 */
                u32 nblk = cnt / 8;
                int r = fs_mount_disk(lba, nblk);
                if (r == 0) {
                    uart_puts("[fs  ] 已挂载磁盘分区（持久存储） 起始LBA=");
                    uart_putdec(lba); uart_puts(" 块数="); uart_putdec(fs_blocks());
                    uart_puts("\n");
                    /* 纯 ASCII 标记：自动化测试只认这行（中文在部分宿主环境下会被降级成 '?'） */
                    uart_puts("[fs  ] MOUNTED-DISK lba=");
                    uart_putdec(lba); uart_puts(" blocks="); uart_putdec(fs_blocks());
                    uart_puts("\n");
                    fs_ready = 1;
                } else {
                    /* 空白数据分区（新盘/未格式化）→ 首次使用自动格式化。
                       只对「全零分区头」这么做；非零却挂不上（文件系统损坏）绝不自动格，
                       避免误删数据 —— 回落内存盘并提示。 */
                    static u8 zsec[512];
                    int blank = (vb_read(lba, zsec, 1) == 0);
                    for (int k = 0; k < 512 && blank; k++) if (zsec[k]) blank = 0;
                    if (blank) {
                        uart_puts("[fs  ] 数据分区是空白（首次使用），自动格式化 ...\n");
                        int f = fs_format_disk(lba, nblk);
                        if (f == 0) f = fs_mount_disk(lba, nblk);
                        if (f == 0) {
                            uart_puts("[fs  ] MOUNTED-DISK lba=");
                            uart_putdec(lba); uart_puts(" blocks="); uart_putdec(fs_blocks());
                            uart_puts(" (formatted)\n");
                            fs_ready = 1;
                        }
                    }
                    if (!fs_ready) {
                        uart_puts("[fs  ] 磁盘分区存在但无法挂载(r=");
                        uart_putdec((u32)(-r)); uart_puts(")，改用内存盘\n");
                    }
                }
                break;
            }
        }
    }
    if (!fs_ready) {
        fs_init();
        uart_puts("[fs  ] 内存盘 1MB 格式化完成，/ /etc /bin /home + 演示文件已建\n");
    }
    /* fs 自检：root inode 与关键路径（宿主机同款断言，隔离内核 vs fs 逻辑） */
    uart_puts("[fs  ] root type="); uart_putdec((u32)fs_filetype(FS_ROOT));
    uart_puts(" size="); uart_putdec((u32)fs_filesize(FS_ROOT));
    uart_puts("  lookup(/home)="); uart_puthex((u64)(u32)fs_lookup("/home"));
    uart_puts(" lookup(/etc/hostname)="); uart_puthex((u64)(u32)fs_lookup("/etc/hostname"));
    uart_puts("\n");

    /* 阶段5：身份三件套（passwd/shadow/demo 家目录），开机即 root */
    fs_setuid(0);
    auth_init();
    uart_puts("[auth] /etc/passwd + /etc/shadow（SHA-256 迭代哈希 x50000）+ /home/demo 已建\n");
    uart_puts("[auth] 用户: root(0) / demo(1000)；默认口令见文档（教学演示），whoami 看身份，su 切换\n");

    /* 阶段5②：把内嵌的用户程序 ELF 写进内存盘 /bin（开机即有，run 直接跑） */
    seed_userprogs();
    uart_puts("[user] /bin/hello /bin/bad /bin/hang /bin/cat /bin/args 已装进内存盘（内嵌 ELF，run <路径> 可跑）\n");

    /* 任务表：task0=本内核（shell 所在，帧在第一次抢占时自然生成）；task1=造帧进入 worker */
    tasks[0].stack_top = (u64)_stack_top;
    tasks[0].name = "kernel/shell";
    tasks[1].stack_top = (u64)&t1_stack[sizeof(t1_stack)];   /* ⚠️ 不赋值 task_create 会拿 0-288 当帧地址 */
    task_create(1, task1_main, "worker");

    u64 freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    timer_init(10);                        /* 10Hz = 100ms 一拍 */
    uart_puts("[time] Generic Timer freq="); uart_puthex(freq);
    uart_puts("Hz，10Hz 抢占式轮转\n");

    /* 阶段6①：USB 键盘（xHCI + HID boot）。找不到设备/没插控制器都只打印一行，不影响串口 shell */
    usb_kbd_init();

    uart_puts("[shell] help=命令清单；试试 ls /  cat /home/readme.txt  whoami  su demo\n> ");
#ifdef BENCH_AT_BOOT
    cmd_bench(0);                 /* 测量构建：开机自动跑一遍基准，不依赖键盘输入 */
#endif
    shell_loop();
}
