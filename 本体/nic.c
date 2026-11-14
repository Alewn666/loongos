#include "loongos.h"
#include "nic.h"
/* LoongOS 阶段10 —— 多网卡调度核心：PCI 枚举 + 驱动注册 + 共享 DMA 池 + 活动网卡分派
 *
 * 这是 net.c 与具体网卡驱动之间的「总线」：
 *   - nic_scan()    枚举 PCI 上所有 network controller（class 0x02），按 vendor/device 选驱动；
 *   - nic_init_all()/attach 让每张卡复位、读完 MAC、探明链路；
 *   - 只有「活动网卡」才从共享池分配收发环与缓冲（避免 8 张卡各自吃 BSS）；
 *   - net.c 全程只跟 nic_active_*() 打交道，不知道底下是哪种型号。
 *
 * ⚠️ 共享池 + 活动占用是关键内存约束：内核 BSS 余量极小，绝不能为每张卡各开一套缓冲。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---------------- 共享 DMA 池（仅活动网卡占用）---------------- */
#define NIC_POOL_SZ (48 * 1024)
static u8  g_nic_pool[NIC_POOL_SZ] __attribute__((aligned(4096)));
static u32 g_pool_off;

void nic_pool_reset(void) { g_pool_off = 0; }

/* 从池里切一块 size 字节、align 对齐的内存，返回物理地址（VA==PA 恒等映射）。失败返回 0。 */
u64 nic_pool_alloc(u32 size, u32 align)
{
    if (align < 1) align = 1;
    u64 base = (u64)g_nic_pool + g_pool_off;
    u64 aligned = (base + align - 1) & ~((u64)align - 1);
    u32 off = (u32)(aligned - (u64)g_nic_pool);
    if (off + size > NIC_POOL_SZ) {
        kputs("[nic ] DMA 池不足（请求 "); kputdec(size); kputs(" align "); kputdec(align);
        kputs("），活动网卡缓冲分配失败\n");
        return 0;
    }
    g_pool_off = off + size;
    return aligned;
}

/* ---------------- PCI ECAM 访问（与 pci.c 同布局）---------------- */
#define NIC_ECAM_BASE 0x4010000000ULL
#define NIC_VENDOR    0x00
#define NIC_CMD       0x04
#define NIC_BAR0      0x10

static u32 ecam_rd(int b, int d, int f, int r)
{
    return *(volatile u32 *)(NIC_ECAM_BASE + ((u64)b << 20) + ((u64)d << 15)
                             + ((u64)f << 12) + (r & 0xffc));
}

/* 挑一个「memory-space BAR」作为 MMIO 寄存器窗口基址。
 *
 * ⚠️ 这是多型号网卡适配里最隐蔽的坑：ARM64 没有 I/O 端口空间。
 *    QEMU 的 RTL8139 把 BAR0 注册成 I/O、BAR1 注册成 MMIO（同一组寄存器的别名）：
 *       pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO,    &s->bar_io);
 *       pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar_mem);
 *    若照搬「取 BAR0」，拿到的是 I/O 地址，当内存去解引用时那个地址上没有任何
 *    memory region，读回全是 0xFF —— 表现为 MAC=ff:ff:ff:ff:ff:ff、复位永不完成、
 *    链路 down（阶段10 验收点 4 就是卡在这里）。
 *
 *    PCI BAR bit0=1 表示 I/O space BAR，bit0=0 表示 memory space BAR。所以这里遍历
 *    6 个 BAR，跳过 I/O BAR，返回**第一个** memory BAR 的基址：
 *       e1000/e1000e : BAR0 = MMIO           → 取 BAR0（与原实现一致）
 *       rtl8139      : BAR0 = I/O, BAR1 = MMIO → 取 BAR1（修复）
 *       virtio-net   : 驱动自带 capability BAR 重分配，不依赖本值
 *    64 位 memory BAR（typ==2）占两个槽，需要拼高 32 位并跳过高半槽。
 *    BAR 已由 pci_enum()（或 UEFI 固件）分配好，这里只读不写。 */
static u64 nic_find_mmio(int b, int d, int f)
{
    for (int i = 0; i < 6; i++) {
        int reg = NIC_BAR0 + i * 4;
        u32 orig = ecam_rd(b, d, f, reg);
        if (orig == 0 || orig == 0xffffffffu) continue;   /* 未使用 / 设备不存在 */
        if (orig & 0x1u) continue;                          /* I/O BAR：ARM64 上不可用 */
        u32 typ = (orig & 0x6u) >> 1;
        u64 addr;
        if (typ == 2) {                                     /* 64 位 memory BAR */
            if (i + 1 >= 6) break;
            u64 hi = ecam_rd(b, d, f, reg + 4);
            addr = ((u64)hi << 32) | (u64)(orig & ~0xfu);
            i++;                                             /* 跳过已用的高半槽 */
        } else {
            addr = (u64)(orig & ~0xfu);
        }
        if (addr) return addr;
    }
    return 0;
}

/* 开 MMIO + 总线主控（DMA 必需） */
static void nic_enable_busmaster(int b, int d, int f)
{
    u32 cmd = ecam_rd(b, d, f, NIC_CMD);
    cmd |= (1u << 1) | (1u << 2);
    *(volatile u32 *)(NIC_ECAM_BASE + ((u64)b << 20) + ((u64)d << 15)
                     + ((u64)f << 12) + (NIC_CMD & 0xffc)) = cmd;
}

/* ---------------- 驱动注册表 ---------------- */
extern struct nic_driver drv_virtio;
extern struct nic_driver drv_e1000;
extern struct nic_driver drv_rtl8139;

static struct nic_driver *g_drivers[] = { &drv_virtio, &drv_e1000, &drv_rtl8139, 0 };

/* ---------------- 全局状态 ---------------- */
struct nic  G_nics[NIC_MAX];
int          G_nic_count;
int          G_nic_active = -1;
static int   g_scanned;

/* ---------------- 扫描与 attach ---------------- */
static void attach_by_pci(int b, int d, int f, u32 id)
{
    if (G_nic_count >= NIC_MAX) return;
    u16 vendor = (u16)(id & 0xffff);
    u16 device = (u16)((id >> 16) & 0xffff);
    u32 cls = ecam_rd(b, d, f, 0x08) >> 24;

    for (int i = 0; g_drivers[i]; i++) {
        struct nic_driver *drv = g_drivers[i];
        if (!drv->match(vendor, device, (u8)cls)) continue;
        struct nic *n = &G_nics[G_nic_count];
        n->present = 0;
        n->type = NIC_TYPE_NONE;
        n->bus = b; n->dev = d; n->fn = f;
        n->vendor_id = vendor; n->device_id = device;
        n->link = 0; n->slot = G_nic_count;
        n->reg_base = nic_find_mmio(b, d, f);
        n->reg_io = 0;
        nic_enable_busmaster(b, d, f);
        int r = drv->attach(n);
        if (r != 0) {
            kputs("[nic ] "); kputs(drv->name); kputs(" attach 失败 r=");
            kputdec((u32)(-r)); kputs("\n");
            return;                                       /* 不计入，留给下一个驱动尝试 */
        }
        /* attach 里通常含一次设备软件复位，QEMU 部分设备复位路径会清掉 PCI COMMAND
           的 Bus Master Enable。e1000 的 e1000x_rx_ready() 要求 BME 才肯收包（virtio/
           rtl8139 不检查），所以复位之后再补一次 MSE+BME，确保 DMA 与收包都被允许。 */
        nic_enable_busmaster(b, d, f);
        n->present = 1;
        G_nic_count++;
        return;
    }
}

/* DFS 枚举一条总线，发现网络控制器（class 0x02）并下钻桥 */
static void scan_bus(int bus)
{
    for (int d = 0; d < 32; d++) {
        u32 v = ecam_rd(bus, d, 0, NIC_VENDOR);
        if ((v & 0xffff) == 0xffff) continue;
        int mf = (ecam_rd(bus, d, 0, 0x0C) >> 16) & 0x80;
        for (int f = 0; f < (mf ? 8 : 1); f++) {
            u32 id = ecam_rd(bus, d, f, NIC_VENDOR);
            if ((id & 0xffff) == 0xffff) continue;
            u32 cls = ecam_rd(bus, d, f, 0x08) >> 24;
            if (cls == 0x06) {                            /* 桥：下钻 secondary 总线 */
                u32 sec = (ecam_rd(bus, d, f, 0x18) >> 8) & 0xff;
                if (sec) scan_bus((int)sec);
            } else if (cls == 0x02) {                     /* network controller */
                attach_by_pci(bus, d, f, id);
            }
        }
    }
}

void nic_scan(void)
{
    if (g_scanned) return;
    g_scanned = 1;
    G_nic_count = 0;
    pci_enum();                                          /* 确保资源分配过（UEFI 或自举） */
    scan_bus(0);
}

int nic_init_all(void)
{
    nic_scan();
    return G_nic_count;
}

int nic_init_count(void) { return G_nic_count; }

/* ---------------- 活动网卡选择 ---------------- */
void nic_select_default(void)
{
    int best = -1;
    for (int i = 0; i < G_nic_count; i++)
        if (G_nics[i].present && G_nics[i].link) { best = i; break; }
    if (best < 0 && G_nic_count > 0) best = 0;
    if (best >= 0) nic_select(best);
}

int nic_select(int idx)
{
    if (idx < 0 || idx >= G_nic_count || !G_nics[idx].present) return -1;
    struct nic *n = &G_nics[idx];
    struct nic_driver *drv = 0;
    for (int i = 0; g_drivers[i]; i++)
        if (g_drivers[i]->match(n->vendor_id, n->device_id, 0x02)) { drv = g_drivers[i]; break; }
    if (!drv) return -1;
    nic_pool_reset();
    int r = drv->activate(n);
    if (r != 0) {
        kputs("[nic ] 激活 "); kputs(n->model); kputs(" 失败 r="); kputdec((u32)(-r)); kputs("\n");
        G_nic_active = -1;
        return r;
    }
    G_nic_active = idx;
    /* bind vtable（net.c 也可直接用这些，但本层优先用统一派发） */
    n->drv_link = drv->link;
    n->drv_send = drv->send;
    n->drv_poll = drv->poll;
    return 0;
}

/* ---------------- 活动网卡便捷派发 ---------------- */
int nic_active_present(void)
{
    return (G_nic_active >= 0 && G_nic_active < G_nic_count && G_nics[G_nic_active].present);
}
int nic_active_link(void)
{
    if (G_nic_active < 0) return 0;
    struct nic *n = &G_nics[G_nic_active];
    return n->drv_link ? n->drv_link(n) : 0;
}
void nic_active_mac(u8 out[6])
{
    if (G_nic_active < 0) { for (int i = 0; i < 6; i++) out[i] = 0; return; }
    struct nic *n = &G_nics[G_nic_active];
    for (int i = 0; i < 6; i++) out[i] = n->mac[i];
}
const char *nic_active_model(void)
{
    if (G_nic_active < 0 || G_nic_active >= G_nic_count) return "?";
    const char *m = G_nics[G_nic_active].model;
    return m ? m : "?";
}
int nic_active_send(const u8 *frame, int len)
{
    if (G_nic_active < 0) return -1;
    struct nic *n = &G_nics[G_nic_active];
    return n->drv_send ? n->drv_send(n, frame, len) : -1;
}
int nic_active_poll(int (*cb)(const u8 *, int))
{
    if (G_nic_active < 0) return 0;
    struct nic *n = &G_nics[G_nic_active];
    return n->drv_poll ? n->drv_poll(n, cb) : 0;
}

/* ---------------- 列出全部网卡 ---------------- */
static void mac_fmt(const u8 m[6])
{
    for (int i = 0; i < 6; i++) {
        if (i) kputs(":");
        char hx[3]; hx[0] = "0123456789abcdef"[m[i] >> 4];
        hx[1] = "0123456789abcdef"[m[i] & 15]; hx[2] = 0;
        kputs(hx);
    }
}

void nic_list_print(void)
{
    kputs("网卡清单（"); kputdec((u32)G_nic_count); kputs(" 张）:\n");
    if (G_nic_count == 0) { kputs("  （无任何 NIC，检查 PCI 拓扑 / 是否 -device 网卡）\n"); return; }
    for (int i = 0; i < G_nic_count; i++) {
        struct nic *n = &G_nics[i];
        kputs("  ["); kputdec((u32)i); kputs("] ");
        kputs(n->model ? n->model : "?");
        kputs("  PCI "); kputdec((u32)n->bus); kputs(":"); kputdec((u32)n->dev);
        kputs("."); kputdec((u32)n->fn);
        kputs("  MAC "); mac_fmt(n->mac);
        kputs(n->link ? "  link up" : "  link down");
        if (i == G_nic_active) kputs("  <== 活动");
        kputs("\n");
    }
    if (G_nic_active < 0)
        kputs("  当前无活动网卡（未初始化或未联网）\n");
}
