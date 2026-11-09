#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS 阶段7 —— virtio-net 网卡驱动（modern virtio-pci，VIRTIO 1.0）
 *
 * 与 virtio_blk.c 同一套路：modern MMIO BAR + 能力链 + VERSION_1 特征协商。
 * 差别：
 *   - 设备 ID：transitional 0x1000 / modern-only 0x1041
 *   - **两个队列**：0 = RX（设备写入）、1 = TX（设备读出）
 *   - 每个包前后带 12 字节 virtio_net 头（VERSION_1 固定 12 字节，全 0 = 不做卸载）
 *   - RX 预贴缓冲：上电就把 8 个可写缓冲挂进队列，设备来包就填一个
 *   - 全程轮询 used 环，不用中断（单核 shell 场景够用且稳）
 *
 * ⚠️ MMIO 一律 rd8/rd16/rd32/rd64 + wr8/wr16/wr32/wr64 单次访问（同 virtio_blk.c 的教训，别用普通指针）。
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

/* ---------------- 状态 ---------------- */
static int  vn_ok;
static u64  vn_common, vn_notify, vn_isr, vn_devcfg;
static u32  vn_notify_mul;
static u16  vn_qnum = VN_QNUM;
static u16  vn_rx_notify_off, vn_tx_notify_off;
static u8   vn_mac[6];
static int  vn_link_up;
static u64  vn_tx_last_used, vn_rx_seen_used;

/* 队列 0 = RX，队列 1 = TX，desc/avail/used 三块独立内存 */
static struct vdesc vn_rx_desc[VN_QNUM] __attribute__((aligned(4096)));
static struct { u16 flags, idx; u16 ring[VN_QNUM]; u16 used_event; } vn_rx_avail __attribute__((aligned(4096)));
static struct { u16 flags, idx; struct vused_elem ring[VN_QNUM]; u16 avail_event; } vn_rx_used __attribute__((aligned(4096)));

static struct vdesc vn_tx_desc[VN_QNUM] __attribute__((aligned(4096)));
static struct { u16 flags, idx; u16 ring[VN_QNUM]; u16 used_event; } vn_tx_avail __attribute__((aligned(4096)));
static struct { u16 flags, idx; struct vused_elem ring[VN_QNUM]; u16 avail_event; } vn_tx_used __attribute__((aligned(4096)));

/* RX 缓冲：每块 = 12 字节头 + 帧区；TX 缓冲同布局 */
static u8 vn_rx_buf[VN_RXBUF][VN_HDRLEN + VN_PKTMAX] __attribute__((aligned(16)));
static u8 vn_tx_buf[VN_HDRLEN + VN_PKTMAX] __attribute__((aligned(16)));

/* ---------------- PCI 小工具（同 virtio_blk.c） ---------------- */
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

/* ---------------- 初始化 ---------------- */
int vn_init(void)
{
    vn_ok = 0;
    int bus = -1, dev = -1, fn = -1;
    for (int b = 0; b < 8 && bus < 0; b++)
        for (int d = 0; d < 32 && bus < 0; d++)
            for (int f = 0; f < 8; f++) {
                u32 id = cfg_rd(b, d, f, PCI_VENDOR);
                if ((id & 0xffff) != VIRTIO_VENDOR) continue;
                u32 did = (id >> 16) & 0xffff;
                if (did != VIRTIO_NET_TRANS && did != VIRTIO_NET_MODERN) continue;
                bus = b; dev = d; fn = f;
                break;
            }
    if (bus < 0) return -1;

    /* BAR 分地址（MMIO 窗口内与 blk 设备隔开：blk 用 +0x100000，net 用 +0x200000） */
    u64 bars[6];
    for (int i = 0; i < 6; i++) {
        u32 sz = bar_size(bus, dev, fn, i);
        if (!sz) { bars[i] = 0; continue; }
        u64 a = bar_assign(bus, dev, fn, i, PCI_MMIO_WINDOW + 0x200000u + (u32)dev * 0x10000u + (u32)i * 0x4000u);
        bars[i] = a;
        if (((cfg_rd(bus, dev, fn, PCI_BAR0 + i * 4) & 0x6u) >> 1) == 2) i++;
    }

    /* 开内存空间 + 总线主控 */
    u32 cmd = cfg_rd(bus, dev, fn, PCI_CMD);
    cfg_wr(bus, dev, fn, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);

    /* 能力链 */
    u32 st = cfg_rd(bus, dev, fn, 0x04) >> 16;
    if (!((st >> 4) & 1)) { kputs("[net ] 设备没有能力链（不支持 modern）\n"); return -2; }
    u32 cap = cfg_rd(bus, dev, fn, PCI_CAP_PTR) & 0xff;
    vn_common = vn_notify = vn_isr = vn_devcfg = 0;
    for (int guard = 0; guard < 48 && cap; guard++) {
        u32 d0 = cfg_rd(bus, dev, fn, (int)(cap & 0xfc));
        u8  id      = (u8)(d0 & 0xff);
        u32 nxt     = (d0 >> 8) & 0xff;
        u32 cfgtype = (d0 >> 24) & 0xff;
        u32 d1 = cfg_rd(bus, dev, fn, (int)((cap + 4) & 0xfc));
        u32 baridx = d1 & 0xff;
        u32 off = cfg_rd(bus, dev, fn, (int)((cap + 8) & 0xfc));
        if (id == 0x09) {
            u64 base = bars[baridx & 7];
            if (base) {
                if (cfgtype == VIRTIO_PCI_CAP_COMMON_CFG)      vn_common = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    vn_notify = base + off;
                    vn_notify_mul = cfg_rd(bus, dev, fn, (int)((cap + 16) & 0xfc));
                }
                else if (cfgtype == VIRTIO_PCI_CAP_ISR_CFG)    vn_isr = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_DEVICE_CFG) vn_devcfg = base + off;
            }
        }
        cap = nxt;
    }
    if (!vn_common || !vn_notify || !vn_devcfg) {
        kputs("[net ] 没找到 virtio 的 common/notify/device 配置\n");
        return -3;
    }

    /* 复位 + 特征协商（只开 VERSION_1；不做校验和/GSO 卸载，全软件算，简单稳） */
    wr8(vn_common + CC_DEV_STATUS, 0);
    bar();
    wr8(vn_common + CC_DEV_STATUS, VST_ACK);
    wr8(vn_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER);
    wr32(vn_common + CC_DEV_FEAT_SEL, 0);
    u32 feat_lo = rd32(vn_common + CC_DEV_FEAT);
    wr32(vn_common + CC_DEV_FEAT_SEL, 1);
    u32 feat_hi = rd32(vn_common + CC_DEV_FEAT);
    u64 devf = ((u64)feat_hi << 32) | feat_lo;
    u64 want = 1ull << VIRTIO_F_VERSION_1;
    if (!(devf & want)) {
        kputs("[net ] 设备不支持 VIRTIO 1.0  feat="); kputhex(devf); kputs("\n");
        return -4;
    }
    wr32(vn_common + CC_DRV_FEAT_SEL, 0);
    wr32(vn_common + CC_DRV_FEAT, (u32)want);
    wr32(vn_common + CC_DRV_FEAT_SEL, 1);
    wr32(vn_common + CC_DRV_FEAT, (u32)(want >> 32));
    wr8(vn_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK);
    if (!(rd8(vn_common + CC_DEV_STATUS) & VST_FEATURES_OK)) {
        kputs("[net ] 设备拒绝 FEATURES_OK\n"); return -5;
    }

    /* 队列数检查 + 两个队列 */
    wr16(vn_common + CC_QUEUE_SEL, 0);
    u16 nq = rd16(vn_common + CC_NUM_QUEUES);
    if (nq < 2) { kputs("[net ] 队列数不足 2\n"); return -6; }

    for (int q = 0; q < 2; q++) {
        wr16(vn_common + CC_QUEUE_SEL, (u16)q);
        u16 qsz = rd16(vn_common + CC_QUEUE_SIZE);
        if (!qsz) { kputs("[net ] 队列不可用\n"); return -7; }
        if (qsz > VN_QNUM) qsz = VN_QNUM;
        vn_qnum = qsz;
        wr16(vn_common + CC_QUEUE_SIZE, qsz);
        struct vdesc *desc = q ? vn_tx_desc : vn_rx_desc;
        void *av = q ? (void *)&vn_tx_avail : (void *)&vn_rx_avail;
        void *us = q ? (void *)&vn_tx_used  : (void *)&vn_rx_used;
        for (u32 i = 0; i < sizeof(vn_rx_desc); i++) ((u8 *)desc)[i] = 0;
        for (u32 i = 0; i < sizeof(vn_rx_avail); i++) ((u8 *)av)[i] = 0;
        for (u32 i = 0; i < sizeof(vn_rx_used); i++) ((u8 *)us)[i] = 0;
        wr64(vn_common + CC_QUEUE_DESC, (u64)desc);
        wr64(vn_common + CC_QUEUE_DRIVER, (u64)av);
        wr64(vn_common + CC_QUEUE_DEVICE, (u64)us);
        wr16(vn_common + CC_QUEUE_ENABLE, 1);
        u16 noff = rd16(vn_common + CC_QUEUE_NOTIFY_OFF);
        if (q == 0) vn_rx_notify_off = noff; else vn_tx_notify_off = noff;
    }

    wr8(vn_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK | VST_DRIVER_OK);
    if (rd8(vn_common + CC_DEV_STATUS) & VST_FAILED) { kputs("[net ] 设备报 FAILED\n"); return -8; }

    /* MAC（device cfg 前 6 字节）+ 链路状态（偏移 6 的 u16，bit0=LINK_UP） */
    for (int i = 0; i < 6; i++) vn_mac[i] = rd8(vn_devcfg + (u64)i);
    u16 lstat = rd16(vn_devcfg + 6);
    vn_link_up = (lstat & 1) ? 1 : 0;

    /* RX 预贴 8 个缓冲：desc[i] 单条可写，avail 全挂上 */
    for (int i = 0; i < VN_RXBUF; i++) {
        vn_rx_desc[i].addr = (u64)vn_rx_buf[i];
        vn_rx_desc[i].len  = VN_HDRLEN + VN_PKTMAX;
        vn_rx_desc[i].flags = VDESC_WRITE;
        vn_rx_desc[i].next = 0;
        vn_rx_avail.ring[i] = (u16)i;
    }
    vn_rx_avail.idx = VN_RXBUF;
    bar();
    { u64 a = vn_notify + (u64)vn_rx_notify_off * (u64)(vn_notify_mul ? vn_notify_mul : 1);
      wr16(a, 0); bar(); }

    vn_tx_last_used = 0;
    vn_rx_seen_used = 0;
    vn_ok = 1;
    return 0;
}

int vn_present(void) { return vn_ok; }
void vn_mac_copy(u8 out[6]) { for (int i = 0; i < 6; i++) out[i] = vn_mac[i]; }
int  vn_link(void) { return vn_link_up; }

/* ---------------- 收 ---------------- */
/* notify：按队列 notify_off 计算地址 */
static void vn_queue_notify(int q)
{
    u16 off = q ? vn_tx_notify_off : vn_rx_notify_off;
    u64 a = vn_notify + (u64)off * (u64)(vn_notify_mul ? vn_notify_mul : 1);
    wr16(a, (u16)q);
    bar();
}

/* 取出所有已到达的 RX 帧，逐个交给回调；返回处理帧数 */
int vn_poll(int (*on_rx)(const u8 *frame, int len))
{
    if (!vn_ok) return 0;
    int n = 0;
    while (vn_rx_seen_used != vn_rx_used.idx) {
        struct vused_elem *e = &vn_rx_used.ring[vn_rx_seen_used % vn_qnum];
        u32 id = e->id;
        u32 len = e->len;
        if (id < VN_RXBUF && len > VN_HDRLEN) {
            int flen = (int)(len - VN_HDRLEN);
            if (flen > VN_PKTMAX) flen = VN_PKTMAX;
            if (on_rx) on_rx(vn_rx_buf[id] + VN_HDRLEN, flen);
            n++;
        }
        /* 缓冲挂回去 */
        vn_rx_avail.ring[vn_rx_avail.idx % vn_qnum] = (u16)id;
        vn_rx_avail.idx++;
        vn_rx_seen_used++;
        bar();
    }
    if (n) vn_queue_notify(0);
    return n;
}

/* ---------------- 发 ---------------- */
int vn_send(const void *frame, int len)
{
    if (!vn_ok) return -1;
    if (len <= 0 || len > VN_PKTMAX) return -2;

    /* 等 TX 环空出来（同步发，一次一帧；shell 场景足够） */
    for (u64 spin = 0; spin < 30000000ull; spin++) {
        bar();
        if (vn_tx_avail.idx - vn_tx_last_used < vn_qnum) break;
        if (spin == 29999999ull) return -3;
    }

    for (int i = 0; i < VN_HDRLEN; i++) vn_tx_buf[i] = 0;   /* 头全 0：不做校验和/GSO 卸载 */
    const u8 *s = (const u8 *)frame;
    for (int i = 0; i < len; i++) vn_tx_buf[VN_HDRLEN + i] = s[i];

    u16 slot = vn_tx_avail.idx % vn_qnum;
    vn_tx_desc[0].addr  = (u64)vn_tx_buf;
    vn_tx_desc[0].len   = (u32)(VN_HDRLEN + len);
    vn_tx_desc[0].flags = 0;                 /* 设备只读 */
    vn_tx_desc[0].next  = 0;
    vn_tx_avail.ring[slot] = 0;
    bar();
    vn_tx_avail.idx++;
    bar();
    vn_queue_notify(1);

    /* 等设备收走（used 环推进） */
    for (u64 spin = 0; spin < 60000000ull; spin++) {
        bar();
        if (vn_tx_used.idx != vn_tx_last_used) {
            vn_tx_last_used = vn_tx_used.idx;
            (void)rd8(vn_isr);
            return 0;
        }
    }
    return -4;
}
