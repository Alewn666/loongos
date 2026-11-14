#include "loongos.h"
#include "nic.h"
/* LoongOS 阶段10 —— virtio-net 网卡驱动（modern virtio-pci，VIRTIO 1.0）· 多实例版
 *
 * 与旧版区别：旧版是「单实例单设备」，所有静态全局只服务一张卡；
 * 现在按 slot（= 该卡在 G_nics[] 的下标）存状态，收发环与缓冲在 activate 时
 * 从**共享 DMA 池**分配（只有活动网卡占用），从而支持多张 virtio-net 并存，
 * 且不把内核 BSS 顶穿。协议流程（能力链 / VERSION_1 协商 / 双队列 / RX 预贴）不变。
 *
 * ⚠️ MMIO 一律 rd8/rd16/rd32/rd64 + wr* 单次访问（同 virtio_blk.c 的教训，别用普通指针）。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---------------- PCI（与 virtio_blk.c 同参） ---------------- */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_MMIO_WINDOW 0x10000000UL
#define PCI_VENDOR      0x00
#define PCI_BAR0        0x10
#define PCI_CAP_PTR     0x34
#define PCI_CMD         0x04
#define PCI_CMD_MSE     (1u << 1)
#define PCI_CMD_BME     (1u << 2)

#define VIRTIO_VENDOR      0x1AF4
#define VIRTIO_NET_TRANS   0x1000
#define VIRTIO_NET_MODERN  0x1041

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define CC_DEV_FEAT_SEL     0x00
#define CC_DEV_FEAT         0x04
#define CC_DRV_FEAT_SEL     0x08
#define CC_DRV_FEAT         0x0c
#define CC_NUM_QUEUES       0x12
#define CC_DEV_STATUS       0x14
#define CC_QUEUE_SEL        0x16
#define CC_QUEUE_SIZE       0x18
#define CC_QUEUE_ENABLE     0x1c
#define CC_QUEUE_NOTIFY_OFF 0x1e
#define CC_QUEUE_DESC       0x20
#define CC_QUEUE_DRIVER     0x28
#define CC_QUEUE_DEVICE     0x30

#define VST_ACK         1
#define VST_DRIVER      2
#define VST_DRIVER_OK   4
#define VST_FEATURES_OK 8
#define VST_FAILED      0x80
#define VIRTIO_F_VERSION_1 32ull

#define VDESC_NEXT  (1u << 0)
#define VDESC_WRITE (1u << 1)

#define VN_QNUM     32          /* 每队列描述符数（够用，BSS 小） */
#define VN_RXBUF    8           /* RX 预贴缓冲个数 */
#define VN_PKTMAX   1514        /* 以太网帧上限（不含 FCS） */
#define VN_HDRLEN   12          /* VERSION_1 的 virtio_net 头 */

struct vdesc { u64 addr; u32 len; u16 flags; u16 next; };
struct vused_elem { u32 id; u32 len; };

struct vq {
    struct vdesc *desc;
    struct { u16 flags, idx; u16 ring[VN_QNUM]; u16 used_event; } *avail;
    struct { u16 flags, idx; struct vused_elem ring[VN_QNUM]; u16 avail_event; } *used;
};

/* ---------------- MMIO ---------------- */
static inline u8 rd8(u64 a)   { u8 v;  __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u16 rd16(u64 a) { u16 v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u32 rd32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u64 rd64(u64 a) { u64 v; __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void wr8(u64 a, u8 v)   { __asm__ volatile("strb %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void wr16(u64 a, u16 v) { __asm__ volatile("strh %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void wr32(u64 a, u32 v) { __asm__ volatile("str %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void wr64(u64 a, u64 v) { __asm__ volatile("str %1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void bar(void)          { __asm__ volatile("dsb sy" ::: "memory"); }

/* ---------------- 每实例（slot）状态 ---------------- */
static int   vn_ok[NIC_MAX];
static u64   vn_common[NIC_MAX], vn_notify[NIC_MAX], vn_isr[NIC_MAX], vn_devcfg[NIC_MAX];
static u32   vn_notify_mul[NIC_MAX];
static u16   vn_qnum[NIC_MAX];
static u16   vn_rx_notify_off[NIC_MAX], vn_tx_notify_off[NIC_MAX];
static u8    vn_mac[NIC_MAX][6];
static int   vn_link_up[NIC_MAX];
static u64   vn_tx_last_used[NIC_MAX], vn_rx_seen_used[NIC_MAX];

/* activate 时从共享池分配出来的环/缓冲（仅活动卡有效；切卡后会被覆盖，但不影响当前卡） */
static struct vq  vn_rxq[NIC_MAX], vn_txq[NIC_MAX];
static u8   *vn_rx_buf[NIC_MAX];         /* VN_RXBUF 个缓冲的基址（连续） */
static u8   *vn_tx_buf[NIC_MAX];

/* ---------------- PCI 小工具 ---------------- */
static volatile u32 *ecam(int bus, int dev, int fn, int reg)
{
    return (volatile u32 *)(PCI_ECAM_BASE + ((u64)bus << 20) + ((u64)dev << 15)
                            + ((u64)fn << 12) + (reg & 0xffc));
}
static u32 cfg_rd(int b, int d, int f, int r) { return *ecam(b, d, f, r); }
static void cfg_wr(int b, int d, int f, int r, u32 v) { *ecam(b, d, f, r) = v; bar(); }

static u32 bar_size(int b, int d, int f, int idx)
{
    int reg = PCI_BAR0 + idx * 4;
    u32 orig = cfg_rd(b, d, f, reg);
    cfg_wr(b, d, f, reg, 0xffffffffu);
    u32 mask = cfg_rd(b, d, f, reg);
    cfg_wr(b, d, f, reg, orig);
    if (!mask) return 0;
    mask &= ~0xfu;
    return (~mask) + 1;
}

static u64 bar_assign(int b, int d, int f, int idx, u32 want)
{
    int reg = PCI_BAR0 + idx * 4;
    u32 orig = cfg_rd(b, d, f, reg);
    u32 addr = orig & ~0xfu;
    u32 typ = (orig & 0x6u) >> 1;
    if (typ == 2) {                                     /* 64 位 BAR */
        if (addr != 0 && addr < 0xfffff000u)
            return ((u64)cfg_rd(b, d, f, reg + 4) << 32) | (u64)addr;
        cfg_wr(b, d, f, reg, want | (orig & 0xfu));
        cfg_wr(b, d, f, reg + 4, 0);
        return ((u64)cfg_rd(b, d, f, reg + 4) << 32) | (u64)(cfg_rd(b, d, f, reg) & ~0xfu);
    }
    if (addr != 0 && addr < 0xfffff000u) return addr;
    cfg_wr(b, d, f, reg, want | (orig & 0xfu));
    return cfg_rd(b, d, f, reg) & ~0xfu;
}

/* ---------------- match / attach ---------------- */
static int vm_match(u16 vendor, u16 device, u8 base_class)
{
    (void)base_class;
    if (vendor != VIRTIO_VENDOR) return 0;
    if (device != VIRTIO_NET_TRANS && device != VIRTIO_NET_MODERN) return 0;
    return 1;
}

static int vm_attach(struct nic *n)
{
    int slot = n->slot;
    int b = n->bus, d = n->dev, f = n->fn;
    vn_ok[slot] = 0;

    /* 分配各 BAR（MMIO 窗口内与 blk 设备隔开） */
    u64 bars[6];
    for (int i = 0; i < 6; i++) {
        u32 sz = bar_size(b, d, f, i);
        if (!sz) { bars[i] = 0; continue; }
        u64 a = bar_assign(b, d, f, i, PCI_MMIO_WINDOW + 0x200000u + (u32)d * 0x10000u + (u32)i * 0x4000u);
        bars[i] = a;
        if (((cfg_rd(b, d, f, PCI_BAR0 + i * 4) & 0x6u) >> 1) == 2) i++;
    }

    u32 cmd = cfg_rd(b, d, f, PCI_CMD);
    cfg_wr(b, d, f, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);

    /* 能力链 */
    u32 st = cfg_rd(b, d, f, 0x04) >> 16;
    if (!((st >> 4) & 1)) { kputs("[net ] virtio 设备没有能力链（不支持 modern）\n"); return -2; }
    u32 cap = cfg_rd(b, d, f, PCI_CAP_PTR) & 0xff;
    vn_common[slot] = vn_notify[slot] = vn_isr[slot] = vn_devcfg[slot] = 0;
    for (int guard = 0; guard < 48 && cap; guard++) {
        u32 d0 = cfg_rd(b, d, f, (int)(cap & 0xfc));
        u8  id      = (u8)(d0 & 0xff);
        u32 nxt     = (d0 >> 8) & 0xff;
        u32 cfgtype = (d0 >> 24) & 0xff;
        u32 d1 = cfg_rd(b, d, f, (int)((cap + 4) & 0xfc));
        u32 baridx = d1 & 0xff;
        u32 off = cfg_rd(b, d, f, (int)((cap + 8) & 0xfc));
        if (id == 0x09) {
            u64 base = bars[baridx & 7];
            if (base) {
                if (cfgtype == VIRTIO_PCI_CAP_COMMON_CFG)      vn_common[slot] = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    vn_notify[slot] = base + off;
                    vn_notify_mul[slot] = cfg_rd(b, d, f, (int)((cap + 16) & 0xfc));
                }
                else if (cfgtype == VIRTIO_PCI_CAP_ISR_CFG)    vn_isr[slot] = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_DEVICE_CFG) vn_devcfg[slot] = base + off;
            }
        }
        cap = nxt;
    }
    if (!vn_common[slot] || !vn_notify[slot] || !vn_devcfg[slot]) {
        kputs("[net ] virtio 没找到 common/notify/device 配置\n");
        return -3;
    }

    /* 复位 + 特征协商（只开 VERSION_1） */
    wr8(vn_common[slot] + CC_DEV_STATUS, 0); bar();
    wr8(vn_common[slot] + CC_DEV_STATUS, VST_ACK);
    wr8(vn_common[slot] + CC_DEV_STATUS, VST_ACK | VST_DRIVER);
    wr32(vn_common[slot] + CC_DEV_FEAT_SEL, 0);
    u32 feat_lo = rd32(vn_common[slot] + CC_DEV_FEAT);
    wr32(vn_common[slot] + CC_DEV_FEAT_SEL, 1);
    u32 feat_hi = rd32(vn_common[slot] + CC_DEV_FEAT);
    u64 devf = ((u64)feat_hi << 32) | feat_lo;
    u64 want = 1ull << VIRTIO_F_VERSION_1;
    if (!(devf & want)) { kputs("[net ] virtio 设备不支持 VIRTIO 1.0\n"); return -4; }
    wr32(vn_common[slot] + CC_DRV_FEAT_SEL, 0);
    wr32(vn_common[slot] + CC_DRV_FEAT, (u32)want);
    wr32(vn_common[slot] + CC_DRV_FEAT_SEL, 1);
    wr32(vn_common[slot] + CC_DRV_FEAT, (u32)(want >> 32));
    wr8(vn_common[slot] + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK);
    if (!(rd8(vn_common[slot] + CC_DEV_STATUS) & VST_FEATURES_OK)) { kputs("[net ] virtio 拒绝 FEATURES_OK\n"); return -5; }

    wr16(vn_common[slot] + CC_QUEUE_SEL, 0);
    u16 nq = rd16(vn_common[slot] + CC_NUM_QUEUES);
    if (nq < 2) { kputs("[net ] virtio 队列数不足 2\n"); return -6; }

    for (int q = 0; q < 2; q++) {
        wr16(vn_common[slot] + CC_QUEUE_SEL, (u16)q);
        u16 qsz = rd16(vn_common[slot] + CC_QUEUE_SIZE);
        if (!qsz) { kputs("[net ] virtio 队列不可用\n"); return -7; }
        if (qsz > VN_QNUM) qsz = VN_QNUM;
        vn_qnum[slot] = qsz;
        wr16(vn_common[slot] + CC_QUEUE_SIZE, qsz);
        u16 noff = rd16(vn_common[slot] + CC_QUEUE_NOTIFY_OFF);
        if (q == 0) vn_rx_notify_off[slot] = noff; else vn_tx_notify_off[slot] = noff;
        /* 队列寄存器先清零；activate 时再用池地址填 */
        wr64(vn_common[slot] + CC_QUEUE_DESC, 0);
        wr64(vn_common[slot] + CC_QUEUE_DRIVER, 0);
        wr64(vn_common[slot] + CC_QUEUE_DEVICE, 0);
    }
    wr8(vn_common[slot] + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK);

    /* MAC（device cfg 前 6 字节）+ 链路状态（偏移 6 的 u16，bit0=LINK_UP） */
    for (int i = 0; i < 6; i++) vn_mac[slot][i] = rd8(vn_devcfg[slot] + (u64)i);
    u16 lstat = rd16(vn_devcfg[slot] + 6);
    vn_link_up[slot] = (lstat & 1) ? 1 : 0;

    for (int i = 0; i < 6; i++) n->mac[i] = vn_mac[slot][i];
    n->link = vn_link_up[slot];
    n->model = "virtio";
    n->type = NIC_TYPE_VIRTIO;
    vn_ok[slot] = 1;
    return 0;
}

/* ---------------- activate：从共享池分配环/缓冲并使能 ---------------- */
static int vm_activate(struct nic *n)
{
    int slot = n->slot;
    if (!vn_ok[slot]) return -1;

    u32 mul = vn_notify_mul[slot] ? vn_notify_mul[slot] : 1;

    /* 各队列三块结构：desc / avail / used（used 需 4KB 对齐，按 4KB 分配最稳） */
    vn_rxq[slot].desc   = (struct vdesc *)nic_pool_alloc(sizeof(struct vdesc) * VN_QNUM, 16);
    vn_rxq[slot].avail  = (void *)nic_pool_alloc(4096, 4096);     /* 整页，避免对齐漂移 */
    vn_rxq[slot].used   = (void *)nic_pool_alloc(4096, 4096);
    vn_txq[slot].desc   = (struct vdesc *)nic_pool_alloc(sizeof(struct vdesc) * VN_QNUM, 16);
    vn_txq[slot].avail  = (void *)nic_pool_alloc(4096, 4096);
    vn_txq[slot].used   = (void *)nic_pool_alloc(4096, 4096);
    vn_rx_buf[slot]     = (u8 *)nic_pool_alloc((u32)(VN_RXBUF * (VN_HDRLEN + VN_PKTMAX)), 16);
    vn_tx_buf[slot]     = (u8 *)nic_pool_alloc(VN_HDRLEN + VN_PKTMAX, 16);
    if (!vn_rxq[slot].desc || !vn_rxq[slot].avail || !vn_rxq[slot].used ||
        !vn_txq[slot].desc || !vn_txq[slot].avail || !vn_txq[slot].used ||
        !vn_rx_buf[slot] || !vn_tx_buf[slot]) return -8;

    for (int q = 0; q < 2; q++) {
        struct vq *vq = q ? &vn_txq[slot] : &vn_rxq[slot];
        wr16(vn_common[slot] + CC_QUEUE_SEL, (u16)q);
        wr64(vn_common[slot] + CC_QUEUE_DESC, (u64)vq->desc);
        wr64(vn_common[slot] + CC_QUEUE_DRIVER, (u64)vq->avail);
        wr64(vn_common[slot] + CC_QUEUE_DEVICE, (u64)vq->used);
        wr16(vn_common[slot] + CC_QUEUE_ENABLE, 1);
    }

    /* RX 预贴 8 个缓冲 */
    for (int i = 0; i < VN_RXBUF; i++) {
        vn_rxq[slot].desc[i].addr = (u64)(vn_rx_buf[slot] + i * (VN_HDRLEN + VN_PKTMAX));
        vn_rxq[slot].desc[i].len  = VN_HDRLEN + VN_PKTMAX;
        vn_rxq[slot].desc[i].flags = VDESC_WRITE;
        vn_rxq[slot].desc[i].next = 0;
        vn_rxq[slot].avail->ring[i] = (u16)i;
    }
    vn_rxq[slot].avail->idx = VN_RXBUF; bar();
    { u64 a = vn_notify[slot] + (u64)vn_rx_notify_off[slot] * (u64)mul; wr16(a, 0); bar(); }

    vn_tx_last_used[slot] = 0;
    vn_rx_seen_used[slot] = 0;
    wr8(vn_common[slot] + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK | VST_DRIVER_OK);
    if (rd8(vn_common[slot] + CC_DEV_STATUS) & VST_FAILED) { kputs("[net ] virtio 设备报 FAILED\n"); return -9; }
    return 0;
}

static int vm_link(struct nic *n)
{
    int slot = n->slot;
    if (!vn_ok[slot]) return 0;
    u16 lstat = rd16(vn_devcfg[slot] + 6);
    vn_link_up[slot] = (lstat & 1) ? 1 : 0;
    n->link = vn_link_up[slot];
    return vn_link_up[slot];
}

static void vm_queue_notify(int slot, int q)
{
    u16 off = q ? vn_tx_notify_off[slot] : vn_rx_notify_off[slot];
    u64 a = vn_notify[slot] + (u64)off * (u64)(vn_notify_mul[slot] ? vn_notify_mul[slot] : 1);
    wr16(a, (u16)q); bar();
}

/* ---------------- 收 ---------------- */
static int vm_poll(struct nic *n, int (*on_rx)(const u8 *frame, int len))
{
    int slot = n->slot;
    if (!vn_ok[slot]) return 0;
    int cnt = 0;
    while (vn_rx_seen_used[slot] != vn_rxq[slot].used->idx) {
        struct vused_elem *e = &vn_rxq[slot].used->ring[vn_rx_seen_used[slot] % vn_qnum[slot]];
        u32 id = e->id, len = e->len;
        if (id < VN_RXBUF && len > VN_HDRLEN) {
            int flen = (int)(len - VN_HDRLEN);
            if (flen > VN_PKTMAX) flen = VN_PKTMAX;
            if (on_rx) on_rx(vn_rx_buf[slot] + id * (VN_HDRLEN + VN_PKTMAX) + VN_HDRLEN, flen);
            cnt++;
        }
        vn_rxq[slot].avail->ring[vn_rxq[slot].avail->idx % vn_qnum[slot]] = (u16)id;
        vn_rxq[slot].avail->idx++;
        vn_rx_seen_used[slot]++;
        bar();
    }
    if (cnt) vm_queue_notify(slot, 0);
    return cnt;
}

/* ---------------- 发 ---------------- */
static int vm_send(struct nic *n, const u8 *frame, int len)
{
    int slot = n->slot;
    if (!vn_ok[slot]) return -1;
    if (len <= 0 || len > VN_PKTMAX) return -2;

    for (u64 spin = 0; spin < 30000000ull; spin++) {
        bar();
        if (vn_txq[slot].avail->idx - vn_tx_last_used[slot] < vn_qnum[slot]) break;
        if (spin == 29999999ull) return -3;
    }
    for (int i = 0; i < VN_HDRLEN; i++) vn_tx_buf[slot][i] = 0;
    const u8 *s = (const u8 *)frame;
    for (int i = 0; i < len; i++) vn_tx_buf[slot][i + VN_HDRLEN] = s[i];

    u16 slo = (u16)(vn_txq[slot].avail->idx % vn_qnum[slot]);
    vn_txq[slot].desc[0].addr  = (u64)vn_tx_buf[slot];
    vn_txq[slot].desc[0].len   = (u32)(VN_HDRLEN + len);
    vn_txq[slot].desc[0].flags = 0;
    vn_txq[slot].desc[0].next  = 0;
    vn_txq[slot].avail->ring[slo] = 0; bar();
    vn_txq[slot].avail->idx++; bar();
    vm_queue_notify(slot, 1);

    for (u64 spin = 0; spin < 60000000ull; spin++) {
        bar();
        if (vn_txq[slot].used->idx != vn_tx_last_used[slot]) {
            vn_tx_last_used[slot] = vn_txq[slot].used->idx;
            (void)rd8(vn_isr[slot]);
            return 0;
        }
    }
    return -4;
}

struct nic_driver drv_virtio = {
    "virtio", vm_match, vm_attach, vm_activate, vm_link, vm_send, vm_poll,
};
