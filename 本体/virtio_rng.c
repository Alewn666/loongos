/* LoongOS —— virtio-rng 熵源驱动（virtio 随机数字设备，VIRTIO 1.0 modern）
 *
 * 为什么加它（2026-11-08，"补点驱动"）：
 *   内核自带 crypto.c（SHA-256 迭代哈希口令），但随机数靠软件 PRNG，没有真熵源。
 *   virtio-rng 是虚拟机/云主机几乎标配的半虚拟化设备（1af4:1005/1044），挂上它系统就有
 *   真实硬件熵，用于密钥/盐值/UUID 等。这是"磁盘之外"最划算的一个小驱动。
 *
 * 参考实现（GitHub 高 star 项目）：
 *   - Linux  drivers/char/hw_random/virtio-rng.c  —— 用户态视角的 rng 驱动范本
 *   - U-Boot drivers/virtio/                       —— 同样的 modern 配置寄存器读法
 *
 * 协议极简：一条队列(queue 0)；要熵就提交一个「设备可写」描述符指向缓冲，notify，等 used，
 *   设备把随机字节填进缓冲。缓冲≤队列深度一次取一批即可。
 *
 * ⚠️ 为省 BSS，desc/avail/used 三段与数据缓冲全挤在**一个 4KB 页**里（连续、不重叠即可；
 *   QEMU/KVM 不要求 used 环单独页对齐）。MMIO 仍走单次内联访问，规避 KVM stage-2 ISV=0。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

extern void kputs(const char *);
extern void kputhex(u64 v);
extern void kputdec(u32 v);
extern void pci_enum(void);

/* ---------------- PCI ---------------- */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_VENDOR      0x00
#define PCI_CMD         0x04
#define PCI_BAR0        0x10
#define PCI_CAP_PTR     0x34
#define PCI_CMD_MSE     (1u << 1)
#define PCI_CMD_BME     (1u << 2)

#define VIRTIO_VENDOR        0x1AF4
#define VIRTIO_RNG_TRANS     0x1005      /* transitional */
#define VIRTIO_RNG_MODERN    0x1044      /* modern only */

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

/* ---------------- 单页队列布局 ---------------- */
#define RNG_PAGE_SZ 4096
#define RNG_QDEPTH  16
/* 段偏移（同一 4KB 页内连续排布） */
#define OFF_DESC  0                      /* RNG_QDEPTH × 16B = 256 */
#define OFF_AVAIL 512                    /* flags+idx+ring[16]+used_event */
#define OFF_USED  1024                   /* flags+idx+ring[16]×8+avail_event */
#define OFF_DATA  2048                   /* 随机数据缓冲（最多 2048 字节） */
#define RNG_BUF_CAP (RNG_PAGE_SZ - OFF_DATA)

static inline u32 rd32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u64 rd64(u64 a) { u64 v; __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void wr32(u64 a, u32 v) { __asm__ volatile("str %w1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void wr64(u64 a, u64 v) { __asm__ volatile("str %1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void wr16(u64 a, u16 v) { __asm__ volatile("strh %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void wr8(u64 a, u8 v)   { __asm__ volatile("strb %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline u8  rd8(u64 a)  { u8 v;  __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u16 rd16(u64 a) { u16 v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void sbar(void)         { __asm__ volatile("dsb sy" ::: "memory"); }

static int    rng_ok;
static u64    rng_common, rng_notify, rng_isr, rng_devcfg;
static u32    rng_notify_mul, rng_notify_off;
static u8     rng_page[RNG_PAGE_SZ] __attribute__((aligned(4096)));

struct rng_desc { u64 addr; u32 len; u16 flags; u16 next; };
static struct rng_desc *rng_desc = (struct rng_desc *)(rng_page + OFF_DESC);

/* ---------------- PCI BFS（同 scsi/nvme） ---------------- */
static volatile u32 *ecam(int bus, int dev, int fn, int reg)
{
    return (volatile u32 *)(PCI_ECAM_BASE + ((u64)bus << 20) + ((u64)dev << 15)
                            + ((u64)fn << 12) + (reg & 0xffc));
}
static u32 cfg_rd(int b, int d, int f, int r) { return *ecam(b, d, f, r); }
static void cfg_wr(int b, int d, int f, int r, u32 v) { *ecam(b, d, f, r) = v; sbar(); }

static int rng_find(int *ob, int *od, int *of)
{
    int buses[64];
    static int visited[256];
    for (int i = 0; i < 256; i++) visited[i] = 0;
    int head = 0, tail = 0;
    buses[tail++] = 0; visited[0] = 1;
    while (head < tail) {
        int b = buses[head++];
        for (int d = 0; d < 32; d++) {
            u32 v = cfg_rd(b, d, 0, PCI_VENDOR);
            if ((v & 0xffff) != VIRTIO_VENDOR) continue;
            u32 did = (v >> 16) & 0xffff;
            if (did == VIRTIO_RNG_TRANS || did == VIRTIO_RNG_MODERN) {
                *ob = b; *od = d; *of = 0;
                return 0;
            }
        }
        for (int d = 0; d < 32; d++) {
            u32 v = cfg_rd(b, d, 0, PCI_VENDOR);
            if ((v & 0xffff) == 0xffff) continue;
            if ((cfg_rd(b, d, 0, 0x08) >> 24) != 0x06) continue;   /* 桥 */
            u32 sec = (cfg_rd(b, d, 0, 0x18) >> 8) & 0xff;
            if (!sec || sec >= 0x100) continue;
            if (!visited[sec] && tail < 64) { visited[sec] = 1; buses[tail++] = (int)sec; }
        }
    }
    return -1;
}

/* 取 ≤len 字节真随机，返回实际取到字节数（<=0 表示失败） */
int rng_get(void *buf, u32 len)
{
    if (!rng_ok) return -1;
    if (len > RNG_BUF_CAP) len = RNG_BUF_CAP;
    if (len == 0) return 0;

    u16 *avail = (u16 *)(rng_page + OFF_AVAIL);
    u16 *used  = (u16 *)(rng_page + OFF_USED);
    /* 描述符[0]：纯设备可写，指向数据缓冲 */
    rng_desc[0].addr = (u64)(rng_page + OFF_DATA);
    rng_desc[0].len  = len;
    rng_desc[0].flags = VDESC_WRITE;
    rng_desc[0].next = 0;
    sbar();

    u16 slot = avail[1] % RNG_QDEPTH;
    avail[2 + slot] = 0;                  /* ring 头 = desc[0] */
    sbar();
    avail[1]++;                           /* idx++ */
    sbar();
    wr16(rng_notify + (u64)rng_notify_off * (rng_notify_mul ? rng_notify_mul : 1), 0);
    sbar();

    u16 before = used[1];
    for (u64 spin = 0; spin < 60000000ull; spin++) {
        sbar();
        if (used[1] != before) {
            (void)rd32(rng_isr);
            u8 *d = (u8 *)buf;
            for (u32 i = 0; i < len; i++) d[i] = rng_page[OFF_DATA + i];
            return (int)len;
        }
    }
    return -2;                            /* 超时 */
}

int rng_init(void)
{
    pci_enum();
    rng_ok = 0;
    int bus = -1, dev = -1, fn = -1;
    if (rng_find(&bus, &dev, &fn) != 0) {
        kputs("[rng] 未找到 virtio-rng 设备(1af4:1005/1044)\n");
        return -1;
    }
    kputs("[rng] 找到设备 bus="); kputdec((u32)bus);
    kputs(" dev="); kputdec((u32)dev); kputs("\n");

    u32 lo = cfg_rd(bus, dev, fn, PCI_BAR0);
    u32 hi = cfg_rd(bus, dev, fn, PCI_BAR0 + 4);
    u64 bars[6];
    if ((lo & 0x6u) == 0x4u) bars[0] = ((u64)hi << 32) | (u64)(lo & 0xfffffff0u);
    else bars[0] = (u64)(lo & 0xfffffff0u);
    for (int i = 1; i < 6; i++) {
        u32 l = cfg_rd(bus, dev, fn, PCI_BAR0 + i * 4);
        if (i > 0 && (cfg_rd(bus, dev, fn, PCI_BAR0 + (i - 1) * 4) & 0x6u) == 0x4u) { bars[i] = 0; continue; }
        if ((l & 0x6u) == 0x4u) {
            u32 h = cfg_rd(bus, dev, fn, PCI_BAR0 + (i + 1) * 4);
            bars[i] = ((u64)h << 32) | (u64)(l & 0xfffffff0u);
        } else bars[i] = (u64)(l & 0xfffffff0u);
    }
    u32 cmd = cfg_rd(bus, dev, fn, PCI_CMD);
    cfg_wr(bus, dev, fn, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);

    if (!((cfg_rd(bus, dev, fn, 0x04) >> 16) & (1u << 4))) { kputs("[rng] 无能力链\n"); return -2; }
    u32 cap = cfg_rd(bus, dev, fn, PCI_CAP_PTR) & 0xff;
    rng_common = rng_notify = rng_isr = rng_devcfg = 0;
    for (int guard = 0; guard < 48 && cap; guard++) {
        u32 d0 = cfg_rd(bus, dev, fn, (int)(cap & 0xfc));
        u8 id = (u8)(d0 & 0xff);
        u32 nxt = (d0 >> 8) & 0xff;
        u32 cfgtype = (d0 >> 24) & 0xff;
        u32 d1 = cfg_rd(bus, dev, fn, (int)((cap + 4) & 0xfc));
        u32 baridx = d1 & 0xff;
        u32 off = cfg_rd(bus, dev, fn, (int)((cap + 8) & 0xfc));
        if (id == 0x09) {
            u64 base = bars[baridx & 7];
            if (base) {
                if (cfgtype == VIRTIO_PCI_CAP_COMMON_CFG)      rng_common = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    rng_notify = base + off;
                    rng_notify_mul = cfg_rd(bus, dev, fn, (int)((cap + 16) & 0xfc));
                }
                else if (cfgtype == VIRTIO_PCI_CAP_ISR_CFG)    rng_isr = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_DEVICE_CFG) rng_devcfg = base + off;
            }
        }
        cap = nxt;
    }
    if (!rng_common || !rng_notify || !rng_devcfg) { kputs("[rng] 缺配置区\n"); return -3; }
    {
        u64 c = rng_common, n = rng_notify, d = rng_devcfg;
        if (!((c >= 0x10000000ull && c < 0x3f000000ull) || (c >= 0x8000000000ull)) ||
            !((n >= 0x10000000ull && n < 0x3f000000ull) || (n >= 0x8000000000ull)) ||
            !((d >= 0x10000000ull && d < 0x3f000000ull) || (d >= 0x8000000000ull))) {
            kputs("[rng] 拒绝可疑 MMIO\n"); return -10;
        }
    }

    wr8(rng_common + CC_DEV_STATUS, 0); sbar();
    { int t = 0; while (rd8(rng_common + CC_DEV_STATUS) != 0 && t < 1000000) t++; }
    wr8(rng_common + CC_DEV_STATUS, VST_ACK);
    wr8(rng_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER);
    wr32(rng_common + CC_DRV_FEAT_SEL, 0); wr32(rng_common + CC_DRV_FEAT, 0);   /* rng 不要特性 */
    wr32(rng_common + CC_DRV_FEAT_SEL, 1); wr32(rng_common + CC_DRV_FEAT, 0);
    wr8(rng_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK);
    if (!(rd8(rng_common + CC_DEV_STATUS) & VST_FEATURES_OK)) { kputs("[rng] FEATURES_OK 被拒\n"); return -5; }

    wr16(rng_common + CC_QUEUE_SEL, 0);
    u16 qsz = rd16(rng_common + CC_QUEUE_SIZE);
    if (!qsz) { kputs("[rng] 队列不可用\n"); return -6; }
    if (qsz > RNG_QDEPTH) qsz = RNG_QDEPTH;
    wr16(rng_common + CC_QUEUE_SIZE, qsz);
    /* 三段队列地址（同一页内偏移） */
    wr64(rng_common + CC_QUEUE_DESC,   (u64)rng_desc);
    wr64(rng_common + CC_QUEUE_DRIVER, (u64)(rng_page + OFF_AVAIL));
    wr64(rng_common + CC_QUEUE_DEVICE, (u64)(rng_page + OFF_USED));
    wr16(rng_common + CC_QUEUE_ENABLE, 1);
    rng_notify_off = rd16(rng_common + CC_QUEUE_NOTIFY_OFF);

    wr8(rng_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK | VST_DRIVER_OK);
    if (rd8(rng_common + CC_DEV_STATUS) & VST_FAILED) { kputs("[rng] 设备 FAILED\n"); return -7; }
    rng_ok = 1;
    kputs("[rng] 就绪\n");
    return 0;
}

int rng_present(void) { return rng_ok; }
