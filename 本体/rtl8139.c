#include "loongos.h"
#include "nic.h"
/* LoongOS 阶段10 —— Realtek RTL8139 网卡驱动（QEMU -device rtl8139 / 10EC:8139）
 *
 * 本驱动严格按 QEMU hw/net/rtl8139.c 的模型语义实现。与「教科书 8139」不同、
 * 曾让多网卡验收卡住的关键点（全部踩过一遍）：
 *
 *  1) BAR：QEMU 把 BAR0 注册成 I/O、BAR1 注册成 MMIO（bar_io 的 0x100 字节别名）：
 *       pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO,     &s->bar_io);
 *       pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY,  &s->bar_mem);
 *     ARM64 没有 I/O 端口空间，必须用 BAR1 的 memory BAR。若拿 BAR0 当 MMIO，
 *     那个地址上没有任何 memory region，读回全是 0xFF（MAC=ff:ff...、复位不完成、
 *     链路 down）。由 nic.c 的 nic_find_mmio() 自动跳过 I/O BAR 挑出 BAR1。
 *
 *  2) 发送：TxStatus[0..3] 的 bit13 = TxHostOwns（1 = host 拥有 / NIC 不发；
 *     复位后四个全置 1）。驱动把「低 13 位帧长、bit13=0」写进当前描述符即触发
 *     发送；QEMU 发完置 TxHostOwns|TxStatOK，并把 currTxDesc 在 0..3 间轮换。
 *     ⇒ 驱动必须维护同样的 cur 计数器，且每帧都写对应的 TxAddr[cur]。
 *
 *  3) 接收：包（4 字节头 + 以太帧 + 4 字节 CRC）由 QEMU DMA 写到 RxBuf 指向的
 *     host 内存，不在 MMIO 寄存器空间！头是 4 字节小端：低 16 位 = RSR 状态
 *     （RxStatusOK=0x1），高 16 位 = 帧长 + 4（含 CRC）。
 *
 *  4) 指针 quirk：0x3A(RxBufAddr/CBR) 读回 = 硬件写指针；0x38(RxBufPtr/CAPR)
 *     写 val 时 QEMU 内部做 RxBufPtr = (val + 16) & (size-1)（源码注释 "off by
 *     16"），读回又是 RxBufPtr - 16。⇒ 驱动维护真实读偏移 sw，写 CAPR 时要写
 *     sw - 16，才能让 QEMU 内部的读指针与驱动一致（否则 avail/unread 全错乱）。
 *
 *  5) RCR：bit11 = 16KB 缓冲（8192 << ((RCR>>11)&3)）。不置位就只有 8KB，与
 *     驱动 16KB 环形取模不一致会直接错乱。bit7 不置 —— QEMU 的 rtl8139_RxWrap()
 *     返回的是 (RxConfig & (1<<7))，置位反而启用「over-wrap」线性写（会越界），
 *     不置位才走安全的分段环形写。
 *
 * 收发缓冲在 activate 时从共享 DMA 池分配（只有活动卡占用）。
 * ⚠️ 寄存器宽度不一：CR(0x37) 字节、CAPR/CBR 字、RxBuf/RCR/TCR/Tx* 双字，
 *    必须分宽度访问，混用 dword 会误伤相邻寄存器。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---- 寄存器偏移（对齐 QEMU enum RTL8139_registers）---- */
#define R3_MAC0   0x00U   /* MAC0..5（字节） */
#define R3_TXS0   0x10U   /* TxStatus0..3  @ 0x10/14/18/1C（双字） */
#define R3_TXA0   0x20U   /* TxAddr0..3    @ 0x20/24/28/2C（双字） */
#define R3_RXBUF  0x30U   /* RX DMA 缓冲物理地址（双字） */
#define R3_CR     0x37U   /* 命令寄存器（字节） */
#define R3_CAPR   0x38U   /* RxBufPtr（字，驱动写：软件读指针） */
#define R3_CBR    0x3AU   /* RxBufAddr（字，驱动读：硬件写指针） */
#define R3_IMR    0x3CU
#define R3_ISR    0x3EU
#define R3_TCR    0x40U
#define R3_RCR    0x44U
#define R3_MSR    0x58U   /* 介质状态：bit2 = LinkDown（1 = 链路断） */

/* ChipCmd 位（QEMU enum ChipCmdBits） */
#define R3_CR_RST  0x10U
#define R3_CR_RXEN 0x08U
#define R3_CR_TXEN 0x04U

/* TxStatus 位（QEMU enum TxStatusBits） */
#define R3_TX_HOST_OWNS 0x2000U   /* 1 = host 拥有 / NIC 不发 */
#define R3_TX_STAT_OK   0x8000U
#define R3_TX_SIZE_MASK 0x1FFFU

/* RxStatus 位（QEMU enum RxStatusBits） */
#define R3_RX_STATUS_OK 0x0001U

/* RCR 位（QEMU enum RxConfigBits） */
#define R3_RCR_AAP     0x0001U   /* AcceptAllPhys：混杂，收所有包 */
#define R3_RCR_APM     0x0002U   /* AcceptMyPhys */
#define R3_RCR_AM      0x0004U   /* AcceptMulticast */
#define R3_RCR_AB      0x0008U   /* AcceptBroadcast */
#define R3_RCR_DMA_UNL 0x0700U   /* RxCfgDMAUnlimited = 7 << 8 */
#define R3_RCR_16K     0x0800U   /* RxCfgRcv16K = 1 << 11 → 8192 << 1 = 16KB */

#define R3_RXBUF_SZ 16384U   /* 必须与 RCR 的 16KB 一致，且为 2 的幂（环形取模） */
#define R3_RX_GUARD  2048U    /* 环形越界保护垫（不参与 DMA 寻址） */
#define R3_TXBUF_SZ 2048U

static inline u8  rd8(u64 a)  { u8 v;  __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u16 rd16(u64 a) { u16 v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u32 rd32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void wr8(u64 a, u8 v)   { __asm__ volatile("strb %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void wr16(u64 a, u16 v) { __asm__ volatile("strh %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void wr32(u64 a, u32 v) { __asm__ volatile("str %w1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void dsb(void)          { __asm__ volatile("dsb sy" ::: "memory"); }

/* ---- 每实例状态（只有活动卡才占 DMA 池）---- */
static int   r3_ok[NIC_MAX];
static u64   r3_base[NIC_MAX];
static u8   *r3_rxb[NIC_MAX];        /* 16KB RX 缓冲 + 2KB 保护垫 */
static u8   *r3_txb[NIC_MAX];        /* 2KB TX 缓冲 */
static u64   r3_txphys[NIC_MAX];
static u16   r3_sw[NIC_MAX];          /* 驱动维护的真实 RX 读偏移（0..R3_RXBUF_SZ） */
static u8    r3_txcur[NIC_MAX];       /* 当前 TX 描述符 0..3，与 QEMU currTxDesc 同步 */

static u64 reg(struct nic *n, u64 off) { return r3_base[n->slot] + off; }

/* 抵消 QEMU CAPR 的 +16 quirk：写 (sw-16) 使内部 RxBufPtr == sw */
static inline void r3_set_capr(u64 base, u16 sw)
{
    wr16(base + R3_CAPR, (u16)(sw - 16u));
}

static int r3_match(u16 vendor, u16 device, u8 base_class)
{
    (void)base_class;
    return (vendor == 0x10EC && device == 0x8139) ? 1 : 0;
}

static int r3_attach(struct nic *n)
{
    int slot = n->slot;
    r3_ok[slot] = 0;
    r3_base[slot] = n->reg_base;
    if (!r3_base[slot]) return -1;

    u64 cb = reg(n, R3_CR);

    /* 软件复位：置 RST。QEMU 立即复位，并在下次读之前把 RST 位清掉 */
    wr8(cb, R3_CR_RST);
    for (int i = 0; i < 200000; i++) if (!(rd8(cb) & R3_CR_RST)) break;
    dsb();

    /* 复位后 QEMU 已从 conf.macaddr 恢复 s->phys，直接读 MAC0..5 */
    for (int i = 0; i < 6; i++) n->mac[i] = rd8(reg(n, R3_MAC0) + i);

    /* 链路：MediaStatus 读回 0xd0(link up) / 0xd4(link down)，bit2 = LinkDown */
    n->link = (rd8(reg(n, R3_MSR)) & 0x04u) ? 0 : 1;
    n->model = "rtl8139";
    n->type = NIC_TYPE_RTL8139;
    r3_ok[slot] = 1;
    return 0;
}

static int r3_activate(struct nic *n)
{
    int slot = n->slot;
    if (!r3_ok[slot]) return -1;
    u64 b = r3_base[slot];

    /* 每次激活都做一次软件复位。attach 阶段的复位只在开机扫描时执行一次，
       但 QEMU rtl8139 的 currTxDesc(0..3 轮换) / RxBufAddr / RxBufPtr / TxStatus[]
       是跨 activate 保留的：切走再切回同一张卡时，驱动虽把 r3_txcur/r3_sw 归零，
       硬件 currTxDesc 却停在上次累计值 —— 驱动写 TxStatus[0] 触发发送，QEMU 却从
       currTxDesc=N 开始检查，帧根本发不出去（表现为「第二次切回该卡 DHCP 失败」）。
       复位把这些内部状态全部归零，保证驱动与硬件指针重新同步。
       （e1000 无此问题：activate 显式写了 TDH/TDT=0。） */
    wr8(b + R3_CR, R3_CR_RST);
    for (int i = 0; i < 200000; i++) if (!(rd8(b + R3_CR) & R3_CR_RST)) break;
    dsb();

    /* 复位会清掉 RxBuf / RxBufferSize(回到 8KB) / 指针，所以这里必须全部重新配置 */
    r3_rxb[slot] = (u8 *)nic_pool_alloc(R3_RXBUF_SZ + R3_RX_GUARD, 16);
    r3_txb[slot] = (u8 *)nic_pool_alloc(R3_TXBUF_SZ, 16);
    if (!r3_rxb[slot] || !r3_txb[slot]) return -2;
    r3_txphys[slot] = (u64)r3_txb[slot];
    r3_sw[slot] = 0;
    r3_txcur[slot] = 0;                 /* 与复位后 QEMU currTxDesc=0 同步 */

    /* 全程轮询，关中断（复位已清 IMR，这里再写一次保证幂等） */
    wr16(b + R3_IMR, 0);

    /* RX DMA 缓冲基址（包写进这块 host 内存，不在 MMIO 空间） */
    wr32(b + R3_RXBUF, (u32)(u64)r3_rxb[slot]);

    /* RCR：收所有包 + DMA 无限 + 16KB 缓冲。写它会 reset_rxring(16384)，
       把 QEMU 内部 RxBufPtr/RxBufAddr 归零 —— 必须放在 CAPR 写入之前。 */
    wr32(b + R3_RCR, (u32)(R3_RCR_AAP | R3_RCR_APM | R3_RCR_AM | R3_RCR_AB |
                            R3_RCR_DMA_UNL | R3_RCR_16K));
    r3_set_capr(b, 0);                  /* 内部 RxBufPtr = 0 */

    /* 四个 TxAddr 都指向同一块 TX 缓冲（每次发送只用当前描述符） */
    for (int i = 0; i < 4; i++)
        wr32(b + R3_TXA0 + (u32)i * 4, (u32)r3_txphys[slot]);

    /* 先开 Rx+Tx：QEMU 的 TxConfig_write 只在 transmitter enabled 时才生效 */
    wr8(b + R3_CR, R3_CR_RXEN | R3_CR_TXEN);
    wr32(b + R3_TCR, 0x03000700u);       /* 经典值（含 CRC 附加 / MXDMA） */
    dsb();
    return 0;
}

static int r3_link(struct nic *n)
{
    int slot = n->slot;
    if (!r3_ok[slot]) return 0;
    n->link = (rd8(reg(n, R3_MSR)) & 0x04u) ? 0 : 1;   /* bit2 = LinkDown，低有效 */
    return n->link;
}

static int r3_poll(struct nic *n, int (*on_rx)(const u8 *frame, int len))
{
    int slot = n->slot;
    if (!r3_ok[slot] || !r3_rxb[slot]) return 0;
    u64 b = r3_base[slot];
    u8 *rxb = r3_rxb[slot];
    const u32 mask = R3_RXBUF_SZ - 1;
    int cnt = 0;
    static u8 tmp[1600];

    for (int guard = 0; guard < 32; guard++) {
        u16 cbr = rd16(b + R3_CBR);       /* 硬件写指针 */
        u16 sw  = r3_sw[slot];            /* 驱动读偏移 */
        if (cbr == sw) break;             /* 缓冲空 */

        /* 4 字节小端头在 DMA 缓冲里（不是 MMIO）：低16=RSR，高16=帧长+4(含CRC) */
        u32 hdr = (u32)rxb[sw & mask]
                | ((u32)rxb[(sw + 1) & mask] << 8)
                | ((u32)rxb[(sw + 2) & mask] << 16)
                | ((u32)rxb[(sw + 3) & mask] << 24);
        u16 rsr = (u16)(hdr & 0xFFFFu);
        u16 len = (u16)(hdr >> 16);       /* 含 4 字节 CRC 的总长 */

        /* 异常长度：直接对齐到硬件写指针，避免死循环 */
        if (len < 4 || len > 1522) {
            r3_sw[slot] = cbr;
            r3_set_capr(b, cbr);
            break;
        }

        if (rsr & R3_RX_STATUS_OK) {
            int flen = (int)len - 4;      /* 剥 CRC */
            if (flen > 1514) flen = 1514;
            u32 off = (u32)((sw + 4) & mask);
            for (int i = 0; i < flen; i++)
                tmp[i] = rxb[(off + (u32)i) & mask];   /* 环形读，跨尾自动回绕 */
            if (on_rx) on_rx(tmp, flen);
            cnt++;
        }

        /* 推进：QEMU 写 4 + size + 4 = len + 4 字节后做 RX_ALIGN 再 MOD2 回环。
           驱动的 sw 必须与硬件写指针用同一套对齐规则，否则下一个头就错位。 */
        u16 nsw = (u16)(((u32)sw + 4u + (u32)len + 3u) & ~3u);
        nsw &= (u16)mask;
        r3_sw[slot] = nsw;
        r3_set_capr(b, nsw);              /* 让 QEMU 内部 RxBufPtr 跟上 */
    }
    return cnt;
}

static int r3_send(struct nic *n, const u8 *frame, int len)
{
    int slot = n->slot;
    if (!r3_ok[slot] || !r3_txb[slot]) return -1;
    if (len <= 0 || len > 1514) return -2;
    u64 b = r3_base[slot];
    int cur = r3_txcur[slot];
    u64 txso = b + R3_TXS0 + (u32)cur * 4;

    /* 等该描述符归还 host（上一帧发完；复位后四个描述符本来就是 host owns） */
    for (u64 spin = 0; spin < 2000000ull; spin++) {
        if (rd32(txso) & R3_TX_HOST_OWNS) break;
        if (spin == 1999999ull) return -3;
    }

    for (int i = 0; i < len; i++) r3_txb[slot][i] = frame[i];
    dsb();

    /* QEMU 复位后 TxAddr 全 0，必须每帧显式写当前描述符的地址 */
    wr32(b + R3_TXA0 + (u32)cur * 4, (u32)r3_txphys[slot]);
    /* 写 TxStatus[cur]：低 13 位帧长 + bit13=0（交给 NIC）→ 触发 rtl8139_transmit() */
    wr32(txso, (u32)((u32)len & R3_TX_SIZE_MASK));
    dsb();

    /* 等发送完成：QEMU 发完会置 TxHostOwns | TxStatOK，并把 currTxDesc 推进 */
    for (u64 spin = 0; spin < 4000000ull; spin++) {
        u32 st = rd32(txso);
        if (st & R3_TX_HOST_OWNS) {
            r3_txcur[slot] = (u8)((cur + 1) & 3);   /* 与 QEMU currTxDesc 同步轮换 */
            return (st & R3_TX_STAT_OK) ? 0 : -4;
        }
        if (spin == 3999999ull) return -5;
    }
    return -5;
}

struct nic_driver drv_rtl8139 = {
    "rtl8139", r3_match, r3_attach, r3_activate, r3_link, r3_send, r3_poll,
};
