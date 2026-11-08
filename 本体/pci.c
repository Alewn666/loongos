/* LoongOS —— 通用 PCIe 枚举与资源分配（裸机 OS 必备）
 *
 * 为什么需要它（2026-11-08）：
 *   之前的驱动假设「固件已经把总线号 / BAR / 桥窗口都配好了」——这在 UEFI 启动下成立，
 *   但裸 ELF 直启（-kernel）、以及部分瘦固件下并不成立：所有 BAR 全是 0、桥的 secondary
 *   总线号是 0，下游设备根本不可达（桥不转发配置周期），驱动全认不到。
 *
 *   真机场景里，玲珑系统从 ISO 经 UEFI 启动，EDK2 会做完整枚举；但让内核**自举枚举**后：
 *     (1) 更稳健——不再依赖固件行为；
 *     (2) 可在 QEMU -kernel 直启下验证所有磁盘/熵驱动（不被 CD-ROM 伪装的 virtio-blk 干扰）；
 *     (3) 多级 PCIe 桥（真机 scsi 盘就在多级桥后）也能被正确发现与寻址。
 *
 *   算法是标准 DFS（同 Linux drivers/pci/probe.c 的 pci_scan_bus / pci_read_bridge_bases）：
 *   自底向上给每条总线上的端点分配 BAR（从一段空闲 32 位 MMIO 窗口连续分配），给桥编程
 *   secondary/subordinate 总线号与 32 位内存窗口。已分配（UEFI 启动）的项一律跳过 → 幂等、
 *   不破坏固件已配好的资源。
 *
 * ⚠️ MMIO 走单次内联访问；分配的 32 位窗口落在 0x10000000-0x3EFFFFFF（内核已按 Device 映射）。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---------------- ECAM 访问（与 virtio_*.c 同布局） ---------------- */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_VENDOR      0x00
#define PCI_CMD         0x04
#define PCI_BAR0        0x10
#define PCI_CAP_PTR     0x34
#define PCI_SUBORD      0x1A      /* 桥：subordinate bus number（[23:16]） */

#define PCI_CMD_MSE     (1u << 1)
#define PCI_CMD_BME     (1u << 2)

static inline u32 rd32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void wr32(u64 a, u32 v) { __asm__ volatile("str %w1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static volatile u32 *ecam(int bus, int dev, int fn, int reg)
{
    return (volatile u32 *)(PCI_ECAM_BASE + ((u64)bus << 20) + ((u64)dev << 15)
                            + ((u64)fn << 12) + (reg & 0xffc));
}
static u32 cfg_rd(int b, int d, int f, int r) { return *ecam(b, d, f, r); }
static void cfg_wr(int b, int d, int f, int r, u32 v) { *ecam(b, d, f, r) = v; dsb(); }

/* 探测某 BAR 大小（写全 1 读回，mask 取反 +1）；返回字节数（0=未用）。
   注意：探测会临时改写当前地址，函数内立即还原，不影响已分配资源。 */
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

/* 给一个 64 位对齐值上取整 */
static inline u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

static int g_next_bus = 1;     /* 从 1 起给未分配的桥分配 secondary 总线号 */
static u32 g_mmio = 0;         /* 32 位 MMIO 分配游标 */

/* 给总线 b 上某个端点设备的所有 BAR 分配地址；更新 lo/hi 为该设备占用的 MMIO 范围
   ⚠️ 幂等守卫：固件（UEFI）已分配过 BAR 的设备，绝不改写（改写会破坏已映射地址与桥窗口，
       导致桥后设备的 MMIO 落到桥不转发的 32 位窗口 → 读全 FF、复位卡死）。判断标准：
       任一 BAR 的地址位（mask 掉类型位）非零即视为已分配。 */
static void assign_endpoint(int b, int d, int f, u32 *lo, u32 *hi)
{
    {
        int assigned = 0;
        for (int i = 0; i < 6; i++)
            if ((cfg_rd(b, d, f, PCI_BAR0 + i * 4) & ~0xfu) != 0u) { assigned = 1; break; }
        if (assigned) {                       /* 已分配：只开 MSE+BME，改写 BAR */
            u32 cmd = cfg_rd(b, d, f, PCI_CMD);
            cfg_wr(b, d, f, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);
            return;
        }
    }
    for (int i = 0; i < 6; i++) {
        u32 sz = bar_size(b, d, f, i);
        if (!sz) continue;
        /* 64 位 BAR：合并高半，下两个寄存器槽不独立处理 */
        int is64 = ((cfg_rd(b, d, f, PCI_BAR0 + i * 4) & 0x6u) == 0x4u);
        u32 addr = align_up(g_mmio, sz > 16 ? sz : 16);
        cfg_wr(b, d, f, PCI_BAR0 + i * 4, addr | 0x0u);   /* 32 位MMIO 地址（高半写 0） */
        if (is64) cfg_wr(b, d, f, PCI_BAR0 + i * 4 + 4, 0);
        if (addr < *lo) *lo = addr;
        if (addr + sz > *hi) *hi = addr + sz;
        g_mmio = addr + sz;
        if (is64) i++;                                       /* 跳过已用的高半槽 */
    }
    /* 开 MMIO + 总线主控（DMA） */
    u32 cmd = cfg_rd(b, d, f, PCI_CMD);
    cfg_wr(b, d, f, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);
}

/* 递归枚举一条总线；返回该总线子树用到的 MMIO 范围[lo,hi)，没用到则 lo>hi */
static void enum_bus(int bus, u32 *lo, u32 *hi)
{
    u32 local_lo = 0xFFFFFFFFu, local_hi = 0;
    u32 dev_mmio_lo, dev_mmio_hi;
    for (int d = 0; d < 32; d++) {
        u32 v = cfg_rd(bus, d, 0, PCI_VENDOR);
        if ((v & 0xffff) == 0xffff) continue;
        int mf = (cfg_rd(bus, d, 0, 0x0C) >> 16) & 0x80;     /* 多功能设备位 */
        for (int f = 0; f < (mf ? 8 : 1); f++) {
            u32 vid = cfg_rd(bus, d, f, PCI_VENDOR);
            if ((vid & 0xffff) == 0xffff) continue;
            u32 cls = cfg_rd(bus, d, f, 0x08) >> 24;          /* base class */
            if (cls == 0x06) {                                 /* 桥（PCI/PCIe） */
                u32 sec = (cfg_rd(bus, d, f, 0x18) >> 8) & 0xff;
                if (!sec) {                                    /* 固件没分配 → 自己分配 */
                    sec = (u32)g_next_bus++;
                    if (sec < 0x100)
                        cfg_wr(bus, d, f, 0x18, ((u32)bus & 0xffu) | ((u32)sec << 8) | (0xFFu << 16));
                }
                if (sec >= 0x100) continue;
                enum_bus((int)sec, &dev_mmio_lo, &dev_mmio_hi);
                if (dev_mmio_lo < dev_mmio_hi) {
                    /* 对齐到 1MB 给桥内存窗口，并把游标推过这段 */
                    u32 win_end = align_up(dev_mmio_hi, 0x100000u);
                    /* 32 位内存窗口 base/limit（bits[31:20] 进寄存器，bit0=1 标 32 位空间） */
                    u32 base  = (dev_mmio_lo >> 16) & 0xFFF0u;
                    u32 limit = ((dev_mmio_hi - 1) >> 16) & 0xFFF0u;
                    cfg_wr(bus, d, f, 0x20, base | 0x1u);     /* memory base（低 16 位） */
                    cfg_wr(bus, d, f, 0x22, limit | 0x1u);    /* memory limit */
                    cfg_wr(bus, d, f, 0x24, 0);               /* memory base 高 32 位 = 0 */
                    cfg_wr(bus, d, f, 0x26, 0);               /* memory limit 高 32 位 = 0 */
                    if (dev_mmio_lo < local_lo) local_lo = dev_mmio_lo;
                    if (win_end > local_hi) local_hi = win_end;
                    g_mmio = win_end;
                }
            } else {
                assign_endpoint(bus, d, f, &dev_mmio_lo, &dev_mmio_hi);
                if (dev_mmio_lo < local_lo) local_lo = dev_mmio_lo;
                if (dev_mmio_hi > local_hi) local_hi = dev_mmio_hi;
            }
        }
    }
    *lo = local_lo; *hi = local_hi;
}

static int g_enum_done;

/* 对外入口：做一次完整枚举（幂等，已分配项跳过）。驱动初始化前调用一次即可。 */
void pci_enum(void)
{
    if (g_enum_done) return;
    g_enum_done = 1;
    /* ⚠️ 幂等：固件（UEFI）已枚举的情况一律跳过，绝不重写固件配好的资源。
       判定：bus0 上任意桥已分配 secondary 总线号，或任意端点已有非零 BAR（任一 BAR 地址位非零）
       = 固件已枚举 → 整轮跳过。裸 ELF 直启 / 瘦固件下 BAR 全 0、桥 secondary=0 → 走自举分配。
       注意不能只查 BAR0：virtio 等设备的 BAR0 可能是 I/O 端口条（mask 掉类型位为 0），会漏判。 */
    for (int d = 0; d < 32; d++) {
        u32 v = cfg_rd(0, d, 0, PCI_VENDOR);
        if ((v & 0xffff) == 0xffff) continue;
        u32 cls = cfg_rd(0, d, 0, 0x08) >> 24;
        if (cls == 0x06) {
            u32 sec = (cfg_rd(0, d, 0, 0x18) >> 8) & 0xff;
            if (sec) return;                          /* 桥已分配 → 固件已枚举 */
        } else {
            int assigned = 0;
            for (int i = 0; i < 6; i++)
                if ((cfg_rd(0, d, 0, PCI_BAR0 + i * 4) & ~0xfu) != 0u) { assigned = 1; break; }
            if (assigned) return;                     /* 端点已有 BAR → 固件已枚举 */
        }
    }
    g_next_bus = 1;
    g_mmio = 0x10000000u + 0x00800000u;     /* 从 32 位窗口内 8MB 处起分配，避开低位平台设备 */
    u32 lo, hi;
    enum_bus(0, &lo, &hi);
}
