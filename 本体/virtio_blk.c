/* LoongOS 阶段6③ —— virtio-blk 磁盘驱动（**modern** virtio-pci，VIRTIO 1.0）
 *
 * 为什么用 modern 而不是 legacy：
 *   legacy 的寄存器落在 **I/O 端口空间**。PCIe 的 I/O 窗口要靠根端口转发，
 *   而 `-kernel` 直载没有固件去配根端口的 I/O base/limit → 读回来全是 0xFF，
 *   驱动「能分配 BAR 却读不到寄存器」（2026-10-25 实测卡在这）。
 *   modern 用**内存 BAR**（MMIO），和 xHCI 同一条路，稳。
 *   顺带：libvirt/QEMU 默认的 transitional 设备（1af4:1001）也带 modern 能力，够用。
 *
 * ⚠️ MMIO 一律 rd8/rd16/rd32/rd64 + wr8/wr16/wr32/wr64（内联汇编单次访问）：
 *   循环里被强度削减成后索引 ldr/str（写回基址）时，stage-2 的 ISV=0 会让 KVM
 *   走 NISV 路径、QEMU 解不出访问 → 给 guest 注入 data abort（第十四轮踩过）。
 *
 * 队列（modern）：desc / avail / used 是**三块独立内存**（不必像 legacy 那样挤在一个 4KB 页里），
 * 地址分别写进 common cfg 的 queue_desc / queue_driver / queue_device。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

extern void kputs(const char *s);
extern void kputhex(u64 v);
extern void kputdec(u32 v);

/* ---------------- PCI ---------------- */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_MMIO_WINDOW 0x10000000UL     /* QEMU virt 的 32 位 MMIO 窗口 */
#define PCI_VENDOR      0x00
#define PCI_STATUS      0x06
#define PCI_BAR0        0x10
#define PCI_CAP_PTR     0x34
#define PCI_CMD         0x04

#define PCI_CMD_MSE     (1u << 1)
#define PCI_CMD_BME     (1u << 2)

#define VIRTIO_VENDOR        0x1AF4
#define VIRTIO_BLK_TRANS     0x1001      /* transitional：legacy+modern 都有 */
#define VIRTIO_BLK_MODERN    0x1042      /* modern only */

/* virtio_pci_cap 的 cfg_type */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

/* common cfg 寄存器偏移（VIRTIO 1.0 §4.1.4.3） */
/* ⚠️ VIRTIO 1.0 的 modern common cfg 里，特征寄存器是**选择性读取**：
   先写 device_feature_select(0x00)=0/1，再读 device_feature(0x04) 得到低/高 32 位；
   驱动特征同理：driver_feature_select(0x08) + driver_feature(0x0c)。
   早期版本当成平坦 u64 读（0x00/0x04），拿到的是 select 寄存器和错位数据，
   表现为"设备不支持 VIRTIO 1.0"。 */
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

/* 请求类型 */
#define VB_T_IN   0
#define VB_T_OUT  1

/* 描述符标志 */
#define VDESC_NEXT  (1u << 0)
#define VDESC_WRITE (1u << 1)

struct vdesc { u64 addr; u32 len; u16 flags; u16 next; };
struct vused_elem { u32 id; u32 len; };
struct vb_hdr { u32 type; u32 reserved; u64 sector; };

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
/* ---- legacy 回退（virtio_blk_legacy.c）：QEMU transitional 设备上 modern 读特征会失败 ---- */
/* ---- virtio-scsi 兜底（virtio_scsi.c）：虚拟机把盘挂成 scsi 总线时用这个 ---- */
extern int vs_init(void);
extern int vs_present(void);
extern u64 vs_capacity_bytes(void);
extern int vs_read(u64 lba, void *buf, u32 nsec);
extern int vs_write(u64 lba, const void *buf, u32 nsec);

extern int vbl_init(void);
extern int vbl_request(u32 type, u64 lba, void *buf, u32 nsec, int dev_writes);
extern u64 vbl_capacity_bytes(void);
static int vb_use_legacy;
static int  vb_ok;
static u64  vb_common, vb_notify, vb_isr, vb_devcfg;
static u32  vb_notify_mul;
static u64  vb_cap_sec;          /* 容量（512B 扇区） */
static u16  vb_qnum = 128;
static u16  vb_notify_off;

/* 队列内存（modern：三块独立） */
static struct vdesc vb_desc[128] __attribute__((aligned(4096)));
static struct {
    u16 flags, idx;
    u16 ring[128];
    u16 used_event;
} vb_avail __attribute__((aligned(4096)));
static struct {
    u16 flags, idx;
    struct vused_elem ring[128];
    u16 avail_event;
} vb_used __attribute__((aligned(4096)));

static u8 vb_hdr_buf[16] __attribute__((aligned(16)));
static u8 vb_st_buf[16]  __attribute__((aligned(16)));
static u8 vb_io_buf[4096] __attribute__((aligned(4096)));

/* ---------------- PCI 小工具 ---------------- */
static volatile u32 *ecam(int bus, int dev, int fn, int reg)
{
    return (volatile u32 *)(PCI_ECAM_BASE + ((u64)bus << 20) + ((u64)dev << 15)
                            + ((u64)fn << 12) + (reg & 0xffc));
}
static u32 cfg_rd(int b, int d, int f, int r) { return *ecam(b, d, f, r); }
static void cfg_wr(int b, int d, int f, int r, u32 v) { *ecam(b, d, f, r) = v; bar(); }

/* 探 BAR 大小（返回字节数；写全 1 再读回） */
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

/* 给未分配的 BAR 分一个 MMIO 窗口内的地址；返回最终地址 */
static u64 bar_assign(int b, int d, int f, int idx, u32 want)
{
    int reg = PCI_BAR0 + idx * 4;
    u32 orig = cfg_rd(b, d, f, reg);
    u32 addr = orig & ~0xfu;
    u32 typ = (orig & 0x6u) >> 1;
    /* ⚠️ 64 位 BAR（bits[2:1]=10）必须把高半也读出来拼成完整地址：
       固件通常把 64 位 MMIO 放在 0x8000000000 段（高半=0x80），
       只取低 32 位会得到 0x4000 这种假地址 —— 于是 common cfg 全读到低内存垃圾
       （表现为 feat_lo == feat_hi，误判"设备不支持 VIRTIO 1.0"）。 */
    if (typ == 2) {
        if (addr != 0 && addr < 0xfffff000u)
            return ((u64)cfg_rd(b, d, f, reg + 4) << 32) | (u64)addr;
        cfg_wr(b, d, f, reg, want | (orig & 0xfu));
        cfg_wr(b, d, f, reg + 4, 0);
        return ((u64)cfg_rd(b, d, f, reg + 4) << 32) | (u64)(cfg_rd(b, d, f, reg) & ~0xfu);
    }
    if (addr != 0 && addr < 0xfffff000u) return addr;     /* 固件已分配 */
    cfg_wr(b, d, f, reg, want | (orig & 0xfu));
    return cfg_rd(b, d, f, reg) & ~0xfu;
}

/* ---------------- 初始化 ---------------- */
static int vb_init_modern(void)
{
    vb_ok = 0;
    int bus = -1, dev = -1, fn = -1;
    for (int b = 0; b < 8 && bus < 0; b++)
        for (int d = 0; d < 32 && bus < 0; d++)
            for (int f = 0; f < 8; f++) {
                u32 id = cfg_rd(b, d, f, PCI_VENDOR);
                if ((id & 0xffff) != VIRTIO_VENDOR) continue;
                u32 did = (id >> 16) & 0xffff;
                if (did != VIRTIO_BLK_TRANS && did != VIRTIO_BLK_MODERN) continue;
                bus = b; dev = d; fn = f;
                break;
            }
    if (bus < 0) return -1;

    /* 1. 给所有未分配的 BAR 分地址（MMIO 窗口，每设备隔 64KB） */
    u64 bars[6];
    for (int i = 0; i < 6; i++) {
        u32 sz = bar_size(bus, dev, fn, i);
        if (!sz) { bars[i] = 0; continue; }
        u64 a = bar_assign(bus, dev, fn, i, PCI_MMIO_WINDOW + 0x100000u + (u32)dev * 0x10000u + (u32)i * 0x4000u);
        bars[i] = a;
        /* 64 位 BAR 占两个槽位，下一槽不是独立 BAR */
        if (((cfg_rd(bus, dev, fn, PCI_BAR0 + i * 4) & 0x6u) >> 1) == 2) i++;
    }

    /* 2. 开内存空间 + 总线主控（DMA） */
    u32 cmd = cfg_rd(bus, dev, fn, PCI_CMD);
    cfg_wr(bus, dev, fn, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);

    /* 3. 走能力链找 common/notify/isr/device cfg */
    /* ⚠️ PCI_STATUS 在偏移 0x06，不满足 ECAM 的 4 字节对齐要求：
       必须读 0x04 那个 dword（低 16 = COMMAND，高 16 = STATUS）再右移。 */
    u32 st = cfg_rd(bus, dev, fn, 0x04) >> 16;
    if (!((st >> 4) & 1)) { kputs("[disk] 设备没有能力链（不支持 modern）\n"); return -2; }
    u32 cap = cfg_rd(bus, dev, fn, PCI_CAP_PTR) & 0xff;
    vb_common = vb_notify = vb_isr = vb_devcfg = 0;
    for (int guard = 0; guard < 48 && cap; guard++) {
        /* struct virtio_pci_cap（10 字节起）：
           [0]cap_vndr [1]cap_next [2]cap_len [3]cfg_type [4]bar [5..7]pad [8]offset [12]length */
        u32 d0 = cfg_rd(bus, dev, fn, (int)(cap & 0xfc));
        u8 id      = (u8)(d0 & 0xff);
        u32 nxt    = (d0 >> 8) & 0xff;
        u32 cfgtype = (d0 >> 24) & 0xff;
        u32 d1 = cfg_rd(bus, dev, fn, (int)((cap + 4) & 0xfc));
        u32 baridx = d1 & 0xff;
        u32 off = cfg_rd(bus, dev, fn, (int)((cap + 8) & 0xfc));
        if (id == 0x09) {
            u64 base = bars[baridx & 7];
            if (base) {
                if (cfgtype == VIRTIO_PCI_CAP_COMMON_CFG)      vb_common = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    vb_notify = base + off;
                    vb_notify_mul = cfg_rd(bus, dev, fn, (int)((cap + 16) & 0xfc));
                }
                else if (cfgtype == VIRTIO_PCI_CAP_ISR_CFG)    vb_isr = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_DEVICE_CFG) vb_devcfg = base + off;
            }
        }
        cap = nxt;
    }
    if (!vb_common || !vb_notify || !vb_devcfg) {
        kputs("[disk] 没找到 virtio 的 common/notify/device 配置\n");
        return -3;
    }

    /* 4. 复位 + 特征协商（必须置 VIRTIO_F_VERSION_1） */
    wr8(vb_common + CC_DEV_STATUS, 0);
    bar();
    wr8(vb_common + CC_DEV_STATUS, VST_ACK);
    wr8(vb_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER);
    wr32(vb_common + CC_DEV_FEAT_SEL, 0);
    u32 feat_lo = rd32(vb_common + CC_DEV_FEAT);
    wr32(vb_common + CC_DEV_FEAT_SEL, 1);
    u32 feat_hi = rd32(vb_common + CC_DEV_FEAT);
    u64 devf = ((u64)feat_hi << 32) | feat_lo;
    u64 want = 1ull << VIRTIO_F_VERSION_1;
    if (!(devf & want)) {
        kputs("[disk] 设备不支持 VIRTIO 1.0\n  feat="); kputhex(devf);
        kputs("\n  common="); kputhex(vb_common);
        kputs(" notify="); kputhex(vb_notify);
        kputs("\n  bar0="); kputhex(bars[0]); kputs(" bar1="); kputhex(bars[1]);
        kputs(" bar4="); kputhex(bars[4]);
        kputs("\n  feat_lo="); kputhex(feat_lo); kputs(" feat_hi="); kputhex(feat_hi);
        kputs(" devcfg="); kputhex(vb_devcfg); kputs("\n");
        /* 调试：common 区前 0x40 字节原样 dump（看清 dfselect 写到底有没有落到设备上） */
        kputs("  common[0x00..0x3c]=");
        for (int i = 0; i < 0x40; i += 4) { kputhex(rd32(vb_common + i)); kputs(" "); }
        kputs("\n  bar0="); kputhex(bars[0]);
        kputs(" bar1="); kputhex(bars[1]);
        kputs("\n");
        return -4;
    }
    wr32(vb_common + CC_DRV_FEAT_SEL, 0);
    wr32(vb_common + CC_DRV_FEAT, (u32)want);
    wr32(vb_common + CC_DRV_FEAT_SEL, 1);
    wr32(vb_common + CC_DRV_FEAT, (u32)(want >> 32));
    wr8(vb_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK);
    if (!(rd8(vb_common + CC_DEV_STATUS) & VST_FEATURES_OK)) {
        kputs("[disk] 设备拒绝 FEATURES_OK\n"); return -5;
    }

    /* 5. 队列 0 */
    wr16(vb_common + CC_QUEUE_SEL, 0);
    u16 qsz = rd16(vb_common + CC_QUEUE_SIZE);
    if (!qsz) { kputs("[disk] 队列 0 不可用\n"); return -6; }
    if (qsz > 128) qsz = 128;                    /* 我们只用 128 条 */
    vb_qnum = qsz;
    wr16(vb_common + CC_QUEUE_SIZE, qsz);
    for (u32 i = 0; i < sizeof(vb_desc); i++) ((u8 *)vb_desc)[i] = 0;
    for (u32 i = 0; i < sizeof(vb_avail); i++) ((u8 *)&vb_avail)[i] = 0;
    for (u32 i = 0; i < sizeof(vb_used); i++) ((u8 *)&vb_used)[i] = 0;
    wr64(vb_common + CC_QUEUE_DESC, (u64)vb_desc);
    wr64(vb_common + CC_QUEUE_DRIVER, (u64)&vb_avail);
    wr64(vb_common + CC_QUEUE_DEVICE, (u64)&vb_used);
    wr16(vb_common + CC_QUEUE_ENABLE, 1);
    vb_notify_off = rd16(vb_common + CC_QUEUE_NOTIFY_OFF);

    wr8(vb_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK | VST_DRIVER_OK);
    if (rd8(vb_common + CC_DEV_STATUS) & VST_FAILED) { kputs("[disk] 设备报 FAILED\n"); return -7; }

    /* 6. 容量：device cfg 前 8 字节（512B 扇区） */
    vb_cap_sec = rd64(vb_devcfg);
    if (!vb_cap_sec) { kputs("[disk] 容量为 0\n"); return -8; }
    vb_ok = 1;
    return 0;
}

int  vb_present(void)   { return vb_ok || vs_present(); }
/* 先试 modern，失败自动回退 legacy（QEMU/libvirt 给的 transitional 设备两种接口都可能有） */
int vb_init(void)
{
    int r = vb_init_modern();
    vb_use_legacy = 0;
    if (r == 0) return 0;
    kputs("[disk] modern 初始化失败(r="); kputdec((u32)(-r));
    kputs(")，回退 legacy … ");
    if (vbl_init() == 0) {
        vb_use_legacy = 1;
        kputs("✅ legacy 可用\n");
        return 0;
    }
    kputs("❌ legacy 也不可用\n");
    return r;
}


int  vb_modern_only(void) { return 0; }
u64  vb_capacity_sectors(void) { return vb_ok ? vb_cap_sec : (vs_capacity_bytes() / 512ull); }
u64  vb_capacity_bytes(void)
{
    if (!vb_ok) return vs_present() ? vs_capacity_bytes() : 0;
    return vb_use_legacy ? vbl_capacity_bytes() : vb_cap_sec * 512ull;
}

/* ---------------- 一次同步请求 ---------------- */
static void vb_notify_queue0(void)
{
    u64 a = vb_notify + (u64)vb_notify_off * (u64)(vb_notify_mul ? vb_notify_mul : 1);
    wr16(a, 0);
    bar();
}

static int vb_request(u32 type, u64 lba, void *buf, u32 nsec, int dev_writes)
{
    if (vb_use_legacy) return vbl_request(type, lba, buf, nsec, dev_writes);
    if (!vb_ok) return -1;
    if (nsec == 0 || nsec > 8) return -2;

    struct vb_hdr *h = (struct vb_hdr *)vb_hdr_buf;
    h->type = type; h->reserved = 0; h->sector = lba;
    vb_st_buf[0] = 0xff;
    bar();

    vb_desc[0].addr = (u64)vb_hdr_buf; vb_desc[0].len = 16;
    vb_desc[0].flags = VDESC_NEXT; vb_desc[0].next = 1;
    vb_desc[1].addr = (u64)buf; vb_desc[1].len = nsec * 512;
    vb_desc[1].flags = VDESC_NEXT | (dev_writes ? VDESC_WRITE : 0); vb_desc[1].next = 2;
    vb_desc[2].addr = (u64)vb_st_buf; vb_desc[2].len = 1;
    vb_desc[2].flags = VDESC_WRITE; vb_desc[2].next = 0;

    u16 slot = vb_avail.idx % vb_qnum;
    vb_avail.ring[slot] = 0;          /* 链头 = desc[0] */
    bar();
    vb_avail.idx++;
    bar();
    vb_notify_queue0();

    u16 before = vb_used.idx;
    for (u64 spin = 0; spin < 60000000ull; spin++) {
        bar();
        if (vb_used.idx != before) {              /* 设备把 used.idx 推上来 = 完成 */
            (void)rd8(vb_isr);                /* 清中断状态 */
            return (vb_st_buf[0] == 0) ? 0 : -3;
        }
    }
    return -4;
}

int vb_read(u64 lba, void *buf, u32 nsec)
{
    if (!vb_ok) return vs_present() ? vs_read(lba, buf, nsec) : -1;
    u32 done = 0;
    while (done < nsec) {
        u32 chunk = (nsec - done > 8) ? 8 : (nsec - done);
        int r = vb_request(VB_T_IN, lba + done, vb_io_buf, chunk, 1);
        if (r < 0) return r;
        u8 *d = (u8 *)buf + (u64)done * 512;
        if (d != vb_io_buf)
            for (u32 i = 0; i < chunk * 512; i++) d[i] = vb_io_buf[i];
        done += chunk;
    }
    return 0;
}

int vb_write(u64 lba, const void *buf, u32 nsec)
{
    if (!vb_ok) return vs_present() ? vs_write(lba, buf, nsec) : -1;
    u32 done = 0;
    while (done < nsec) {
        u32 chunk = (nsec - done > 8) ? 8 : (nsec - done);
        const u8 *s = (const u8 *)buf + (u64)done * 512;
        if (s != vb_io_buf)
            for (u32 i = 0; i < chunk * 512; i++) vb_io_buf[i] = s[i];
        int r = vb_request(VB_T_OUT, lba + done, vb_io_buf, chunk, 0);
        if (r < 0) return r;
        done += chunk;
    }
    return 0;
}

/* fs 层用：一块 = 4KB = 8 扇区 */
int vb_read_block(u64 blk_no, u64 part_lba, void *buf)
{
    return vb_read(part_lba + blk_no * 8, buf, 8);
}
int vb_write_block(u64 blk_no, u64 part_lba, const void *buf)
{
    return vb_write(part_lba + blk_no * 8, buf, 8);
}
