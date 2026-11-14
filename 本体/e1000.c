#include "loongos.h"
#include "nic.h"
/* LoongOS 阶段10 —— Intel 8254x / 8257x 千兆网卡驱动（含 QEMU 的 e1000=82540EM、e1000e=82574L）
 *
 * 覆盖用户清单里的 "Intel E1000" 与 "Intel E1000E"：82540EM / 82574L 的发送接收描述符、
 * CTRL/RCTL/TCTL、RAL/RAH 读 MAC 都是同一套寄存器（82574 在此基础上多了 RXDCTL/TXDCTL
 * 队列使能位，本驱动针对 0x10D3 额外置位），所以共用一份代码，只换型号字串。
 *
 * 收发环与缓冲在 activate 时从共享 DMA 池分配（只有活动卡占用），收发用 legacy 16 字节描述符，
 * NIC 自动补 CRC（TX cmd IFCS），RX 这边 RCTL.SECRC 剥 CRC，上层拿到的就是干净以太帧。
 *
 * ⚠️⚠️ TX 与 RX 描述符的字节布局**不一样**，绝不能共用一个结构体（阶段10 踩过的坑）：
 *      Intel/QEMU 的 legacy 描述符都是 16 字节，但字段含义/位置不同：
 *        偏移   0..7        8..9      10      11      12       13       14..15
 *        TX   buffer_addr  length    cso     cmd     status   css      special
 *        RX   buffer_addr  length    checksum(2 字节)  status   errors   special
 *      关键差异：TX 的「命令字节 cmd」在 offset 11（lower.data[31:24]），而 RX 在那里
 *      是 checksum 的高字节；RX 的 status 在 offset 12。早期代码用同一个 e1_desc
 *      （cmd 落在 offset 13）描述两者，结果驱动写 `cmd=0x0B`(EOP|IFCS|RS) 到 offset 13，
 *      QEMU 从 offset 11 读到 0 → 认为没有 EOP → 描述符被「处理」但帧根本没发出去。
 *      表现极具迷惑性：TDH 照常推进、驱动以为发送成功，却永远收不到 DHCP OFFER。
 *      因此下面定义 e1_tx_desc / e1_rx_desc 两个结构体，各自按真实布局排布。
 *
 * ⚠️ MMIO 用单次 rd32/wr32（裸机 + Device 映射，别用普通指针解引用）。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---- 寄存器偏移（8254x / 8257x 通用）---- */
#define E1_CTRL      0x0000UL
#define E1_STATUS    0x0008UL
#define E1_CTRL_EXT  0x0018UL
#define E1_EERD      0x0014UL
#define E1_ICR       0x00C0UL
#define E1_ICS       0x00C8UL
#define E1_IMS       0x00D0UL
#define E1_IMC       0x00D8UL
#define E1_RCTL      0x0100UL
#define E1_TCTL      0x0400UL
#define E1_TIPG      0x0410UL
#define E1_RDBAL     0x2800UL
#define E1_RDBAH     0x2804UL
#define E1_RDLEN     0x2808UL
#define E1_RDH       0x2810UL
#define E1_RDT       0x2818UL
#define E1_RXDCTL    0x2828UL
#define E1_TDBAL     0x3800UL
#define E1_TDBAH     0x3804UL
#define E1_TDLEN     0x3808UL
#define E1_TDH       0x3810UL
#define E1_TDT       0x3818UL
#define E1_TXDCTL    0x3828UL
#define E1_RAL0      0x5400UL
#define E1_RAH0      0x5404UL

/* CTRL 位 */
#define E1_CTRL_RST  0x04000000UL   /* 复位 */
#define E1_CTRL_SLU  0x40UL         /* 强制链路 up（无 PHY / 内部 PHY 时必要） */
/* RCTL 位 */
#define E1_RCTL_EN   0x2UL
#define E1_RCTL_BAM  0x8000UL       /* 收广播 */
#define E1_RCTL_SECRC 0x04000000UL  /* 收包剥 CRC */
/* TCTL 位 */
#define E1_TCTL_EN   0x2UL
#define E1_TCTL_PSP  0x8UL          /* 短包填充 */
/* legacy TX 描述符 cmd 位 */
#define E1_TXD_CMD_EOP   0x01U      /* End Of Packet */
#define E1_TXD_CMD_IFCS  0x02U      /* Insert FCS（硬件补 CRC） */
#define E1_TXD_CMD_RS    0x08U      /* Report Status */
/* legacy RX 描述符 status 位 */
#define E1_RXD_STAT_DD   0x01U      /* Descriptor Done */
/* RXDCTL / TXDCTL 队列使能（82574 需要） */
#define E1_XXDCTL_EN 0x02000000UL

/* ---- legacy 描述符（各 16 字节，布局不同！见文件头注释）---- */
struct __attribute__((packed)) e1_tx_desc {
    u64 buffer_addr;    /* 0..7  */
    u16 length;         /* 8..9  */
    u8  cso;            /* 10    */
    u8  cmd;            /* 11    ← QEMU/Intel 从这里读命令字节 */
    u8  status;         /* 12    */
    u8  css;            /* 13    */
    u16 special;        /* 14..15*/
};
struct __attribute__((packed)) e1_rx_desc {
    u64 buffer_addr;    /* 0..7  */
    u16 length;         /* 8..9  */
    u16 checksum;       /* 10..11*/
    u8  status;         /* 12    ← DD 位 */
    u8  errors;         /* 13    */
    u16 special;        /* 14..15*/
};

static inline u32 rd32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void wr32(u64 a, u32 v) { __asm__ volatile("str %w1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* ---- 每实例状态 ---- */
#define E1_N 8                       /* 收发环描述符数（够用，BSS 小） */
#define E1_RXBUF_SZ 2048
static int   e1_ok[NIC_MAX];
static u64   e1_base[NIC_MAX];
static struct e1_tx_desc *e1_txd[NIC_MAX];
static struct e1_rx_desc *e1_rxd[NIC_MAX];
static u8   *e1_rxb[NIC_MAX];        /* 连续 E1_N 个 RX 缓冲基址 */
static u8   *e1_txb[NIC_MAX];        /* 一个 TX 缓冲 */
static u64   e1_rxphys[NIC_MAX];     /* RX 缓冲物理基址（连续） */
static int   e1_rxhead[NIC_MAX];      /* 下一个待查 RX 描述符 */
static int   e1_txhead[NIC_MAX];      /* 下一个待填 TX 描述符 */

static u32 eerd_read(int slot, u16 word)
{
    u64 a = e1_base[slot] + E1_EERD;
    wr32(a, (1u << 0) | ((u32)word << 2));
    for (int i = 0; i < 200000; i++) {
        u32 v = rd32(a);
        if (v & (1u << 4)) return v >> 16;     /* DONE 位 = bit4，数据在 bit16..31 */
    }
    return 0;
}

static int e1_match(u16 vendor, u16 device, u8 base_class)
{
    if (vendor != 0x8086) return 0;
    if (base_class != 0x02) return 0;
    /* 0x10D3 = 82574L（e1000e）；其余常见 8254x/8257x 千兆 ID 当成 e1000。
       避开 I21x（0x15xx，寄存器不兼容，本驱动不覆盖）。 */
    if (device == 0x10D3) return 1;
    if (device >= 0x100E && device <= 0x10FF) return 1;
    return 0;
}

static int e1_attach(struct nic *n)
{
    int slot = n->slot;
    e1_ok[slot] = 0;
    e1_base[slot] = n->reg_base;
    if (!e1_base[slot]) { kputs("[nic ] e1000 无 MMIO 基址\n"); return -1; }

    u64 cb = e1_base[slot] + E1_CTRL;
    u64 sb = e1_base[slot] + E1_STATUS;

    /* 复位：置 RST，等它自己清掉 */
    wr32(cb, rd32(cb) | (u32)E1_CTRL_RST);
    for (int i = 0; i < 500000; i++) if (!(rd32(cb) & (u32)E1_CTRL_RST)) break;
    dsb();

    /* 强制链路 up（无外置 PHY / 内部 PHY 时必要） */
    wr32(cb, rd32(cb) | (u32)E1_CTRL_SLU);
    dsb();

    /* 关中断（轮询，避免设备乱拉 IRQ） */
    wr32(e1_base[slot] + E1_IMC, 0xFFFFFFFFu);
    (void)rd32(e1_base[slot] + E1_ICR);

    /* 读 MAC：先试 RAL/RAH（QEMU/真实卡复位后自动从 EEPROM 载入）；全 0 再走 EERD */
    u32 ral = rd32(e1_base[slot] + E1_RAL0);
    u32 rah = rd32(e1_base[slot] + E1_RAH0);
    u8 mac[6];
    if (ral == 0 && rah == 0) {
        u32 w0 = eerd_read(slot, 0), w1 = eerd_read(slot, 1), w2 = eerd_read(slot, 2);
        mac[0] = (u8)(w0 & 0xff); mac[1] = (u8)(w0 >> 8);
        mac[2] = (u8)(w1 & 0xff); mac[3] = (u8)(w1 >> 8);
        mac[4] = (u8)(w2 & 0xff); mac[5] = (u8)(w2 >> 8);
    } else {
        mac[0] = (u8)(ral & 0xff); mac[1] = (u8)(ral >> 8);
        mac[2] = (u8)(ral >> 16); mac[3] = (u8)(ral >> 24);
        mac[4] = (u8)(rah & 0xff); mac[5] = (u8)(rah >> 8);
    }
    /* 把 MAC 写回 RAL/RAH 并置 AV，保证地址过滤认我们的包 */
    wr32(e1_base[slot] + E1_RAL0, (u32)mac[0] | ((u32)mac[1] << 8) | ((u32)mac[2] << 16) | ((u32)mac[3] << 24));
    wr32(e1_base[slot] + E1_RAH0, (u32)mac[4] | ((u32)mac[5] << 8) | (1u << 31));
    dsb();

    for (int i = 0; i < 6; i++) n->mac[i] = mac[i];
    u32 st = rd32(sb);
    n->link = (st & 0x2u) ? 1 : 0;       /* STATUS.LU */
    n->model = (n->device_id == 0x10D3) ? "e1000e" : "e1000";
    n->type = (n->device_id == 0x10D3) ? NIC_TYPE_E1000E : NIC_TYPE_E1000;
    e1_ok[slot] = 1;
    return 0;
}

static int e1_activate(struct nic *n)
{
    int slot = n->slot;
    if (!e1_ok[slot]) return -1;
    u64 b = e1_base[slot];

    e1_txd[slot] = (struct e1_tx_desc *)nic_pool_alloc((u32)(sizeof(struct e1_tx_desc) * E1_N), 128);
    e1_rxd[slot] = (struct e1_rx_desc *)nic_pool_alloc((u32)(sizeof(struct e1_rx_desc) * E1_N), 128);
    e1_txb[slot] = (u8 *)nic_pool_alloc(E1_RXBUF_SZ, 16);
    e1_rxb[slot] = (u8 *)nic_pool_alloc((u32)(E1_RXBUF_SZ * E1_N), 16);
    if (!e1_txd[slot] || !e1_rxd[slot] || !e1_txb[slot] || !e1_rxb[slot]) return -2;
    e1_rxphys[slot] = (u64)e1_rxb[slot];   /* VA==PA 恒等映射 */

    /* ---- 收：环 + 缓冲 ---- */
    for (int i = 0; i < E1_N; i++) {
        e1_rxd[slot][i].buffer_addr = e1_rxphys[slot] + (u64)i * E1_RXBUF_SZ;
        e1_rxd[slot][i].length = 0;
        e1_rxd[slot][i].checksum = 0;
        e1_rxd[slot][i].status = 0;
        e1_rxd[slot][i].errors = 0;
        e1_rxd[slot][i].special = 0;
    }
    wr32(b + E1_RDBAL, (u32)(u64)e1_rxd[slot]);
    wr32(b + E1_RDBAH, (u32)((u64)e1_rxd[slot] >> 32));
    wr32(b + E1_RDLEN, (u32)(sizeof(struct e1_rx_desc) * E1_N));
    wr32(b + E1_RDH, 0);
    wr32(b + E1_RDT, E1_N - 1);
    e1_rxhead[slot] = 0;

    /* ---- 发：环 + 缓冲 ---- */
    for (int i = 0; i < E1_N; i++) {
        e1_txd[slot][i].buffer_addr = 0;
        e1_txd[slot][i].length = 0;
        e1_txd[slot][i].cso = 0;
        e1_txd[slot][i].cmd = 0;
        e1_txd[slot][i].status = 0;
        e1_txd[slot][i].css = 0;
        e1_txd[slot][i].special = 0;
    }
    wr32(b + E1_TDBAL, (u32)(u64)e1_txd[slot]);
    wr32(b + E1_TDBAH, (u32)((u64)e1_txd[slot] >> 32));
    wr32(b + E1_TDLEN, (u32)(sizeof(struct e1_tx_desc) * E1_N));
    wr32(b + E1_TDH, 0);
    wr32(b + E1_TDT, 0);
    e1_txhead[slot] = 0;

    /* 82574 需要显式使能收发队列；8254x 写 RCTL/TCTL.EN 即可（多写无害） */
    if (n->device_id == 0x10D3) {
        wr32(b + E1_RXDCTL, (u32)E1_XXDCTL_EN);
        wr32(b + E1_TXDCTL, (u32)E1_XXDCTL_EN);
    }

    /* RCTL：EN + 收广播 + 剥 CRC（缓冲 2048 字节，BSIZE=0） */
    wr32(b + E1_RCTL, (u32)(E1_RCTL_EN | E1_RCTL_BAM | E1_RCTL_SECRC));
    /* TCTL：EN + 短包填充 + CT=0x10 + COLD=0x3F */
    wr32(b + E1_TCTL, (u32)(E1_TCTL_EN | E1_TCTL_PSP | (0x10u << 4) | (0x3Fu << 12)));
    dsb();
    return 0;
}

static int e1_link(struct nic *n)
{
    int slot = n->slot;
    if (!e1_ok[slot]) return 0;
    int up = (rd32(e1_base[slot] + E1_STATUS) & 0x2u) ? 1 : 0;
    n->link = up;
    return up;
}

static int e1_poll(struct nic *n, int (*on_rx)(const u8 *frame, int len))
{
    int slot = n->slot;
    if (!e1_ok[slot]) return 0;
    int cnt = 0;
    u64 b = e1_base[slot];
    for (int guard = 0; guard < E1_N * 2; guard++) {
        struct e1_rx_desc *d = &e1_rxd[slot][e1_rxhead[slot]];
        if (!(d->status & E1_RXD_STAT_DD)) break;      /* DD 未置：没有新包 */
        int len = (int)d->length;
        if (len > E1_RXBUF_SZ) len = E1_RXBUF_SZ;
        if (len > 4 && on_rx) on_rx(e1_rxb[slot] + e1_rxhead[slot] * E1_RXBUF_SZ, len);
        d->status = 0;                                   /* 归还给 NIC */
        e1_rxhead[slot] = (e1_rxhead[slot] + 1) % E1_N;
        wr32(b + E1_RDT, (u32)e1_rxhead[slot] == 0 ? (u32)(E1_N - 1) : (u32)(e1_rxhead[slot] - 1));
        cnt++;
    }
    return cnt;
}

static int e1_send(struct nic *n, const u8 *frame, int len)
{
    int slot = n->slot;
    if (!e1_ok[slot]) return -1;
    if (len <= 0 || len > 1514) return -2;
    u64 b = e1_base[slot];

    /* 等有一个空闲 TX 描述符（TDH 追上 TDT 之前） */
    for (u64 spin = 0; spin < 2000000ull; spin++) {
        u16 tdh = (u16)rd32(b + E1_TDH);
        u16 tdt = (u16)rd32(b + E1_TDT);
        int used = (int)(tdt - tdh) % E1_N;
        if (used < E1_N - 1) break;
        if (spin == 1999999ull) return -3;
    }

    int i = e1_txhead[slot];
    for (int k = 0; k < len; k++) e1_txb[slot][k] = frame[k];
    e1_txd[slot][i].buffer_addr = (u64)e1_txb[slot];
    e1_txd[slot][i].length = (u16)len;
    e1_txd[slot][i].cso = 0;
    e1_txd[slot][i].cmd = (u8)(E1_TXD_CMD_EOP | E1_TXD_CMD_IFCS | E1_TXD_CMD_RS);  /* 0x0B，offset 11 */
    e1_txd[slot][i].status = 0;
    e1_txd[slot][i].css = 0;
    e1_txd[slot][i].special = 0;
    dsb();
    e1_txhead[slot] = (i + 1) % E1_N;
    wr32(b + E1_TDT, (u32)e1_txhead[slot]);   /* 提交；QEMU 同步处理，TDH 随即推进 */
    dsb();

    /* 等本帧发完（TDH 推进到刚提交的槽） */
    u16 want = (u16)((i + 1) % E1_N);
    for (u64 spin = 0; spin < 2000000ull; spin++) {
        if ((u16)rd32(b + E1_TDH) == want) return 0;
        if (spin == 1999999ull) return -4;
    }
    return -4;
}

struct nic_driver drv_e1000 = {
    "e1000", e1_match, e1_attach, e1_activate, e1_link, e1_send, e1_poll,
};
