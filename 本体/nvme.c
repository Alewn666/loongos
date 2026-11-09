#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
#include "blkdev.h"    /* 块设备统一抽象层：导出 ops */
/* LoongOS —— NVMe 磁盘驱动（PCIe NVMe 控制器，NVM Express 1.x）
 *
 * 为什么加它（2026-11-08，"补点驱动"）：
 *   之前的磁盘栈只有 virtio-blk / virtio-scsi 两条 virtio 路线。很多云主机、真实 PCIe
 *   SSD、以及部分 libvirt 默认磁盘类型都是**原生 NVMe**（class=0x010802，不是 virtio），
 *   那些环境里系统会落到内存盘、install 装不进硬盘。补上 NVMe 后，凡是挂了 NVMe 控制器的
 *   机器都能识别、读写、安装。
 *
 * 参考实现（GitHub 高 star 项目，移植其寄存器/队列模型）：
 *   - U-Boot  drivers/nvme.c            —— bootloader 视角的紧凑实现，本文件的主参考
 *   - Linux    drivers/nvme/host/pci.c  —— 队列/PRP/identify 的权威定义
 *   - NVMe 规范 §3（寄存器）、§4.2（提交/完成队列）、§6（admin + NVM 命令集）
 *
 * 与 virtio 的差别：NVMe 是**真 PCIe 设备**，寄存器是内存 BAR0（64 位），命令走「提交队列 +
 *   完成队列」，数据用 PRP（物理区域页）描述。我们只需：
 *     - admin 队列：Identify Controller / Identify Namespace / 创建 IO 队列
 *     - 1 条 IO 队列：Read(0x02) / Write(0x01)
 *   传输≤4KB → 单 PRP1 即可，无需 PRP 链表（省一大块缓冲）。
 *
 * ⚠️ MMIO 全部走 rd32/rd64/wr32 单次内联访问（同 virtio 系列：强度削减产生的后索引
 *   寻址在 KVM stage-2 下会触发 ISV=0 data abort）。DMA 缓冲在 RW 区，CPU 与设备间
 *   必须做缓存维护（dc civac 清写、dc ivac 失效），真实硬件上缺了会数据错位。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;


/* ---------------- PCI ---------------- */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_VENDOR      0x00
#define PCI_CMD         0x04
#define PCI_BAR0        0x10
#define PCI_CMD_MSE     (1u << 1)
#define PCI_CMD_BME     (1u << 2)

#define NVME_CLASS      0x0108u            /* class 寄存器 >>16 后：base=0x01, sub=0x08(NVM) */
#define NVME_VENDOR     0x1d1du            /* 常见：1d1d/8086/2646/144d…，不以厂商筛选，只看 class */

/* ---------------- NVMe 寄存器（相对 BAR0） ---------------- */
#define NVME_CAP        0x00               /* 64 位：能力 */
#define NVME_VS         0x08               /* 32 位：版本 */
#define NVME_CC         0x14               /* 32 位：控制器配置 */
#define NVME_CSTS       0x1C               /* 32 位：控制器状态 */
#define NVME_AQA        0x24               /* 32 位：admin 队列属性 */
#define NVME_ASQ        0x28               /* 64 位：admin 提交队列基址 */
#define NVME_ACQ        0x30               /* 64 位：admin 完成队列基址 */
#define NVME_SQ0DB      0x1000             /* 门铃区起点（队列 0 的 SQ 门铃） */

/* CC 位（严格按 QEMU 8.2.2 include/block/nvme.h 的 enum NvmeCc：
 *   CC_EN_SHIFT=0, CC_CSS_SHIFT=4, CC_MPS_SHIFT=7,
 *   CC_IOSQES_SHIFT=16, CC_IOCQES_SHIFT=20
 * ⚠️ 旧代码写成 `6<<20 | 4<<24`，整体左移了 4 位，是错的。
 *    正确的 IOSQES=6(64B) → bit[19:16]，IOCQES=4(16B) → bit[23:20]。
 *    QEMU 的 nvme_create_cq() 会校验 iosqes==6 && iocqes==4，不符直接报错。 */
#define NVME_CC_EN      (1u << 0)
#define NVME_CC_CSS_NVM 0u                 /* CSS=0 → NVM 命令集（bit[6:4]） */
#define NVME_CC_MPS(mps) (((u32)(mps) & 0xfu) << 7)
#define NVME_CC_IOSQES(sz) (((u32)(sz) & 0xfu) << 16)   /* SQ 条目 = 2^sz 字节 */
#define NVME_CC_IOCQES(sz) (((u32)(sz) & 0xfu) << 20)   /* CQ 条目 = 2^sz 字节 */

/* CSTS 位 */
#define NVME_CSTS_RDY   (1u << 0)
#define NVME_CSTS_CFS   (1u << 1)

/* ---------------- 队列深度（同步单飞，4 条足够） ---------------- */
#define AQ_DEPTH  4
#define IO_DEPTH  4

/* ---------------- MMIO ---------------- */
static inline u32 rd32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u64 rd64(u64 a) { u64 v; __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void wr32(u64 a, u32 v) { __asm__ volatile("str %w1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void wr64_(u64 a, u64 v) { __asm__ volatile("str %1, [%0]" :: "r"(a), "r"(v) : "memory"); }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dc_civac(u64 a) { __asm__ volatile("dc civac, %0" :: "r"(a) : "memory"); }
static inline void dc_ivac(u64 a)  { __asm__ volatile("dc ivac, %0"  :: "r"(a) : "memory"); }

/* ---------------- 状态 ---------------- */
static int   nvme_ok;
static u64   nvme_reg;
static u32   nvme_stride;          /* 门铃步长（字节）：(1<<DSTRD)*4 */
static u32   nvme_page;
static u32   nvme_nsid = 1;
static u64   nvme_cap_sec;         /* 容量（512B 扇区） */
static u32   nvme_block;           /* 逻辑块字节数（通常 512） */

/* ---------------- 队列内存（页对齐是硬性要求，见下） ----------------
 *
 * ⚠️ 2026-11-08 二次修正：之前用 union 把 5 个区域叠在一起，是**错的**。
 *    union 只分配最大成员（dbuf 4096B），asq/acq/iosq/iocq/dbuf 全挤在同一页，
 *    偏移分别是 0/256/320/576/640 —— 除 asq 外全都不满足 4KB 对齐。
 *    QEMU nvme_start_ctrl() 的 "acq & (page_size-1)" 会直接 return -1。
 *
 * QEMU 8.2.2 hw/nvme/ctrl.c 的 nvme_start_ctrl() 明确要求：
 *     if (unlikely(asq & (page_size - 1))) return -1;   ← ASQ 必须页对齐
 *     if (unlikely(acq & (page_size - 1))) return -1;   ← ACQ 必须页对齐
 * 一旦不对齐，控制器置 CSTS.CFS=1（RDY 永不置位），现象就是日志里的
 *     [nvme] 使能后 CSTS=0x2 (RDY=0 CFS=1)
 *     [nvme] 使能后 RDY 未起
 *
 * 另外 nvme_create_cq() / nvme_create_sq() 也要求 PRP1 页对齐，
 * 所以 IO 的 CQ/SQ 同样必须页对齐。
 *
 * 参考高星开源实现（都给**每个**队列单独取整页）：
 *   - U-Boot drivers/nvme/nvme.c  nvme_alloc_queue():
 *         nvmeq->cqes    = memalign(4096, NVME_CQ_ALLOCATION);
 *         nvmeq->sq_cmds = memalign(4096, NVME_SQ_SIZE(depth));
 *   - Linux  drivers/nvme/host/pci.c nvme_alloc_queue():
 *         每个 SQ/CQ 单独 dma_alloc_coherent() 一页以上。
 * 这里没有动态分配器，改为在 .nvmedma 段里按 4KB 偏移静态切开，效果等价。
 *
 * 放在 linker.ld 的 .nvmedma 专用段里（整体 4KB 对齐），而不是 .bss：
 *   - .nvmedma 段在 .bss **之后**，与内核栈（_stack_bottom.._stack_top，
 *     链接器在 .bss 成员之后才分配）彻底分开，不会被栈覆盖；
 *   - 段起点 4KB 对齐 → 每个 4KB 整数倍偏移的分区都 4KB 对齐；
 *   - 分区连续排布 → 物理连续，DMA 友好。
 *
 * 队列条目大小：SQE = 16 个 u32 = 64B（IOSQES=6），CQE = 4 个 u32 = 16B（IOCQES=4）。
 */
#define NV_QPAGE      4096u
#define NV_ASQ_OFF    (0u * NV_QPAGE)
#define NV_ACQ_OFF    (1u * NV_QPAGE)
#define NV_IOSQ_OFF   (2u * NV_QPAGE)
#define NV_IOCQ_OFF   (3u * NV_QPAGE)
#define NV_BUF_OFF    (4u * NV_QPAGE)
#define NV_DMA_PAGES  6u               /* 第 6 页：Identify 的 PRP2 落点（有效内存） */

#define NV_ASQ_SIZE   (AQ_DEPTH * 64)
#define NV_ACQ_SIZE   (AQ_DEPTH * 16)
#define NV_IOSQ_SIZE  (IO_DEPTH * 64)
#define NV_IOCQ_SIZE  (IO_DEPTH * 16)

__attribute__((section(".nvmedma"), aligned(4096)))
static u8 nv_dma[NV_DMA_PAGES * NV_QPAGE];

#define aq_sq   ((volatile u32 *)(nv_dma + NV_ASQ_OFF))
#define aq_cq   ((volatile u32 *)(nv_dma + NV_ACQ_OFF))
#define io_sq   ((volatile u32 *)(nv_dma + NV_IOSQ_OFF))
#define io_cq   ((volatile u32 *)(nv_dma + NV_IOCQ_OFF))
#define nv_buf  (nv_dma + NV_BUF_OFF)

/* CPU 与设备间的缓存维护必须覆盖**整个** DMA 缓冲，不能只清一行。
 * AArch64 的 dc civac / dc ivac 以 cacheline（64B）为粒度作用于给定地址，
 * 旧代码只对 nv_buf 首地址做一次，导致 4096B 的 Identify / IO 数据里
 * 后面 4032 字节可能读到 CPU 脏缓存里的旧值。U-Boot 的
 * nvme_invalidate_cache_aligned() 同样是 invalidate_dcache_range(start, stop)。 */
static void nv_clean_buf(u32 len)
{
    for (u64 a = (u64)nv_buf; a < (u64)nv_buf + len; a += 64) dc_civac(a);
}
static void nv_inv_buf(u32 len)
{
    for (u64 a = (u64)nv_buf; a < (u64)nv_buf + len; a += 64) dc_ivac(a);
}

static int   aq_sq_tail, aq_cq_head, aq_phase = 1;
static int   io_sq_tail, io_cq_head, io_phase = 1;

/* ---------------- PCI 小工具（含多级桥 BFS，与 virtio_scsi.c 同思路） ---------------- */
static volatile u32 *ecam(int bus, int dev, int fn, int reg)
{
    return (volatile u32 *)(PCI_ECAM_BASE + ((u64)bus << 20) + ((u64)dev << 15)
                            + ((u64)fn << 12) + (reg & 0xffc));
}
static u32 cfg_rd(int b, int d, int f, int r) { return *ecam(b, d, f, r); }
static void cfg_wr(int b, int d, int f, int r, u32 v) { *ecam(b, d, f, r) = v; dsb(); }

/* 返回 [bus,dev,fn]，找不到返回 -1；bus/dev/fn 经出参回传 */
static int nv_find_dev(int *ob, int *od, int *of)
{
    int buses[64];
    static int visited[256];
    static int nv_next_bus;
    nv_next_bus = 1;
    for (int i = 0; i < 256; i++) visited[i] = 0;
    int head = 0, tail = 0;
    buses[tail++] = 0; visited[0] = 1;
    while (head < tail) {                       /* BFS 枚举所有桥后的次级总线 */
        int b = buses[head++];
        for (int d = 0; d < 32; d++) {
            u32 v = cfg_rd(b, d, 0, PCI_VENDOR);
            if ((v & 0xffff) == 0xffff) continue;
            u32 cls = cfg_rd(b, d, 0, 0x08) >> 16;   /* [31:16]=base<<8|sub; NVMe=0x0108 */
            if (cls == NVME_CLASS) {
                /* 同类 class 只可能一个 NVMe；但也要把桥继续往下探，免得 NVMe 也在多级桥后 */
                *ob = b; *od = d; *of = 0;
                return 0;
            }
        }
        for (int d = 0; d < 32; d++) {
            u32 v = cfg_rd(b, d, 0, PCI_VENDOR);
            if ((v & 0xffff) == 0xffff) continue;
            u32 bc = cfg_rd(b, d, 0, 0x08) >> 24;    /* base class */
            if (bc != 0x06) continue;                /* 桥 */
            u32 sec = (cfg_rd(b, d, 0, 0x18) >> 8) & 0xff;
            if (!sec) {
                sec = (u32)nv_next_bus++;
                if (sec < 0x100)
                    cfg_wr(b, d, 0, 0x18,
                           ((u32)b & 0xffu) | ((u32)sec << 8) | (0xFFu << 16));
            }
            if (sec >= 0x100) continue;
            if (!visited[sec] && tail < 64) { visited[sec] = 1; buses[tail++] = (int)sec; }
        }
    }
    return -1;
}

/* ---------------- 门铃 ---------------- */
static inline u64 nv_sq_db(int q) { return nvme_reg + NVME_SQ0DB + (u64)q * 2 * nvme_stride; }
static inline u64 nv_cq_db(int q) { return nvme_reg + NVME_SQ0DB + ((u64)q * 2 + 1) * nvme_stride; }

/* ---------------- 同步发一条 admin 命令（cmd 已是 64 字节，cid 固定 0） ----------------
 *
 * ⚠️ 2026-11-08：完成队列条目的解析位置之前是错的，已按 QEMU 8.2.2 修正。
 *
 * QEMU 的 CQE 结构（include/block/nvme.h）：
 *     typedef struct QEMU_PACKED NvmeCqe {
 *         uint32_t result;   // DW0
 *         uint32_t dw1;      // DW1
 *         uint16_t sq_head;  // DW2 低
 *         uint16_t sq_id;    // DW2 高
 *         uint16_t cid;      // DW3 低
 *         uint16_t status;   // DW3 高
 *     } NvmeCqe;
 * 写成 u32[4] 数组后：c[0]=DW0, c[1]=DW1, c[2]=DW2, c[3]=DW3。
 *
 * QEMU 写 CQE 的代码（hw/nvme/ctrl.c:1507）：
 *     req->cqe.status = cpu_to_le16((req->status << 1) | cq->phase);
 * 即 DW3 高 16 位里：bit0 = phase(相位标签)，bit15:1 = status code。
 * 换算到 DW3（u32）上：
 *     phase = (DW3 >> 16) & 1
 *     sc    = (DW3 >> 17) & 0xff        <- 通用状态码 SC
 *     sct   = (DW3 >> 25) & 0x7         <- 状态码类型 SCT
 * 旧代码错读 c[0]（DW0 是 result 字段），所以相位判断永远错乱。
 */
#define NVME_CQE_PHASE(dw3)  (((dw3) >> 16) & 1u)
#define NVME_CQE_SC(dw3)     (((dw3) >> 17) & 0xffu)
#define NVME_CQE_SCT(dw3)    (((dw3) >> 25) & 0x7u)

static int nv_admin(u32 *cmd)
{
    int q = aq_sq_tail;
    for (int i = 0; i < 16; i++) aq_sq[q * 16 + i] = cmd[i];
    dc_civac((u64)&aq_sq[q * 16]);
    aq_sq_tail = (aq_sq_tail + 1) % AQ_DEPTH;
    dsb();
    /* ⚠️ 门铃必须写**递增后的** tail（新 tail），不是刚填的那个槽位号 q。
     * U-Boot nvme_submit_cmd():  ++tail; writel(tail, nvmeq->q_db);
     * QEMU nvme_process_db():     new_tail = val; 处理 [sq->tail, new_tail)
     * 写旧值 q 时 QEMU 认为队列没有新命令 → admin 命令永不完成 → 超时。 */
    wr32(nv_sq_db(0), (u32)aq_sq_tail);
    dsb();
    for (u64 spin = 0; spin < 20000000ull; spin++) {
        dc_ivac((u64)&aq_cq[aq_cq_head * 4]);
        volatile u32 *c = &aq_cq[aq_cq_head * 4];
        u32 dw3 = c[3];                                /* ← DW3，不是 DW0 */
        if (NVME_CQE_PHASE(dw3) == (u32)aq_phase) {   /* 相位匹配 = 完成 */
            u32 sc  = NVME_CQE_SC(dw3);
            u32 sct = NVME_CQE_SCT(dw3);
            int st = (int)((sct << 8) | sc);
            aq_cq_head = (aq_cq_head + 1) % AQ_DEPTH;
            if (aq_cq_head == 0) aq_phase ^= 1;
            dsb();
            wr32(nv_cq_db(0), (u32)aq_cq_head);            /* 回写 CQ 头 */
            return st;                                     /* 0 = 成功 */
        }
    }
    return -1;                                             /* 超时 */
}

/* ---------------- 同步发一条 IO 命令（Read=0x02 / Write=0x01） ---------------- */
static int nv_io(u32 *cmd)
{
    int q = io_sq_tail;
    for (int i = 0; i < 16; i++) io_sq[q * 16 + i] = cmd[i];
    dc_civac((u64)&io_sq[q * 16]);
    io_sq_tail = (io_sq_tail + 1) % IO_DEPTH;
    dsb();
    wr32(nv_sq_db(1), (u32)io_sq_tail);   /* 同样写新 tail，见 nv_admin 注释 */
    dsb();
    for (u64 spin = 0; spin < 20000000ull; spin++) {
        dc_ivac((u64)&io_cq[io_cq_head * 4]);
        volatile u32 *c = &io_cq[io_cq_head * 4];
        u32 dw3 = c[3];                                /* ← DW3 */
        if (NVME_CQE_PHASE(dw3) == (u32)io_phase) {
            u32 sc  = NVME_CQE_SC(dw3);
            u32 sct = NVME_CQE_SCT(dw3);
            int st = (int)((sct << 8) | sc);
            io_cq_head = (io_cq_head + 1) % IO_DEPTH;
            if (io_cq_head == 0) io_phase ^= 1;
            dsb();
            wr32(nv_cq_db(1), (u32)io_cq_head);
            return st;
        }
    }
    return -1;
}

/* ---------------- 小端读取（NVMe Identify 结构全部是小端） ----------------
 * ⚠️ 旧代码用 be32() 读 Identify 数据，这是错的：
 *    NVMe Identify Controller / Namespace 结构里的所有多字节字段都是小端，
 *    QEMU 直接把结构体（cpu_to_le* 填充）memcpy 给控制器，Linux 也是 le*_to_cpu 读。
 */
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

/* ---------------- 初始化 ---------------- */
int nv_init(void)
{
    pci_enum();
    nvme_ok = 0;
    int bus = -1, dev = -1, fn = -1;
    if (nv_find_dev(&bus, &dev, &fn) != 0) {
        kputs("[nvme] 未找到 NVMe 控制器(class=0x010802)\n");
        return -1;
    }
    kputs("[nvme] 找到设备 bus="); kputdec((u32)bus);
    kputs(" dev="); kputdec((u32)dev); kputs(" fn="); kputdec((u32)fn); kputs("\n");

    /* 读 64 位 BAR0（现代 NVMe 必是 64 位内存 BAR） */
    u32 lo = cfg_rd(bus, dev, fn, PCI_BAR0);
    u32 hi = cfg_rd(bus, dev, fn, PCI_BAR0 + 4);
    u64 base = ((u64)hi << 32) | (u64)(lo & 0xfffffff0u);
    if (!base) { kputs("[nvme] BAR0 为空\n"); return -2; }
    /* 安全闸：地址必须落在已知窗口（同 virtio 系列，避免摸不存在的 MMIO → external abort） */
    if (!((base >= 0x10000000ull && base < 0x3f000000ull) || (base >= 0x8000000000ull))) {
        kputs("[nvme] 拒绝可疑 MMIO base="); kputhex(base); kputs("\n");
        return -10;
    }
    nvme_reg = base;

    u32 cmd = cfg_rd(bus, dev, fn, PCI_CMD);
    cfg_wr(bus, dev, fn, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);   /* 开 MMIO + 总线主控(DMA) */

    u64 cap = rd64(nvme_reg + NVME_CAP);
    int dstrd = (int)((cap >> 32) & 0xf);
    int mpsmin = (int)((cap >> 48) & 0xf);
    nvme_stride = (1u << dstrd) * 4u;
    nvme_page  = (1u << (12 + mpsmin));
    u32 mqes = (u32)(cap & 0xffff) + 1;            /* 控制器最大队列深度 */
    if (mqes < AQ_DEPTH || mqes < IO_DEPTH) { kputs("[nvme] 队列深度太小\n"); return -3; }
    u32 to = ((cap >> 24) & 0xff);                  /* 超时（×500ms） */
    kputs("[nvme] cap stride="); kputdec((u32)nvme_stride);
    kputs(" page="); kputdec(nvme_page);
    kputs(" mqes="); kputdec(mqes);
    kputs(" to="); kputdec(to); kputs("\n");

    /* 复位：清 CC.EN，等待 RDY 掉；再配队列、置 EN，等待 RDY 起 */
    wr32(nvme_reg + NVME_CC, rd32(nvme_reg + NVME_CC) & ~NVME_CC_EN);
    dsb();
    u32 t = 0;
    while ((rd32(nvme_reg + NVME_CSTS) & NVME_CSTS_RDY) && t < 1000000) t++;
    if (t >= 1000000) { kputs("[nvme] 复位等待 RDY 掉超时\n"); return -4; }

    /* 清队列内存（避免残留脏命令被设备当有效）—— 整段 nv_dma 一起清，
       再按 cacheline 逐行 clean，保证设备看到的全是 0。 */
    for (u32 i = 0; i < sizeof(nv_dma); i++) nv_dma[i] = 0;
    for (u64 a = (u64)nv_dma; a < (u64)nv_dma + sizeof(nv_dma); a += 64) dc_civac(a);

    /* 页对齐自检：QEMU 的 nvme_start_ctrl() 对 ASQ/ACQ 做 (page_size-1) 掩码检查，
       nvme_create_cq/sq() 对 PRP1 同样检查。不对齐就 CFS=1 → RDY 永不置位。
       这里提前拦下并打印，比等到「使能后 RDY 未起」再猜要直接得多。 */
    if (nvme_page > NV_QPAGE) {
        kputs("[nvme] 控制器要求页大小="); kputdec(nvme_page);
        kputs(" > 静态 4KB 分区，无法满足对齐\n");
        return -12;
    }
    if ((((u64)aq_sq) & (nvme_page - 1)) || (((u64)aq_cq) & (nvme_page - 1)) ||
        (((u64)io_sq) & (nvme_page - 1)) || (((u64)io_cq) & (nvme_page - 1)) ||
        (((u64)nv_buf) & (nvme_page - 1))) {
        kputs("[nvme] 队列未页对齐: asq="); kputhex((u64)aq_sq);
        kputs(" acq="); kputhex((u64)aq_cq);
        kputs(" iosq="); kputhex((u64)io_sq);
        kputs(" iocq="); kputhex((u64)io_cq);
        kputs(" buf="); kputhex((u64)nv_buf); kputs("\n");
        return -12;
    }

    /* admin 队列基址 + 属性
     * AQA: bit[11:0]=ASQS(admin SQ 容量-1), bit[27:16]=ACQS(admin CQ 容量-1) */
    wr64_(nvme_reg + NVME_ASQ, (u64)aq_sq);
    wr64_(nvme_reg + NVME_ACQ, (u64)aq_cq);
    wr32(nvme_reg + NVME_AQA, ((u32)(AQ_DEPTH - 1) << 16) | (u32)(AQ_DEPTH - 1));

    /* CC：EN=1, CSS=NVM(0), MPS=mpsmin, IOSQES=6(64B), IOCQES=4(16B)
     * ⚠️ 位位置严格按 QEMU: IOSQES=bit[19:16], IOCQES=bit[23:20]（旧代码整体左移 4 位） */
    u32 cc = NVME_CC_EN | NVME_CC_CSS_NVM
           | NVME_CC_MPS(mpsmin)
           | NVME_CC_IOSQES(6)
           | NVME_CC_IOCQES(4);
    /* 诊断打印：ASQ/ACQ 的对齐与 CC 的位域是「RDY 起不来」的两个直接原因 */
    kputs("[nvme] ASQ="); kputhex((u64)aq_sq);
    kputs(" ACQ="); kputhex((u64)aq_cq);
    kputs(" CC="); kputhex((u64)cc); kputs("\n");
    wr32(nvme_reg + NVME_CC, cc);
    dsb();
    t = 0;
    while (!(rd32(nvme_reg + NVME_CSTS) & NVME_CSTS_RDY) && t < 5000000) t++;
    {
        u32 csts = rd32(nvme_reg + NVME_CSTS);
        kputs("[nvme] 使能后 CSTS="); kputhex((u64)csts);
        kputs(" (RDY="); kputdec((u32)(csts & 1));
        kputs(" CFS="); kputdec((u32)((csts >> 1) & 1)); kputs(")\n");
    }
    u32 csts_final = rd32(nvme_reg + NVME_CSTS);
    /* CFS 必须先判：QEMU 启动失败时是 CFS=1 + RDY=0，若先判 RDY 就永远
       打印「RDY 未起」，把真正的「配置被拒」原因藏起来了。 */
    if (csts_final & NVME_CSTS_CFS) {
        kputs("[nvme] 控制器报致命错误(CFS)：配置被拒，检查 ASQ/ACQ 对齐、CC 位域、AQA\n");
        return -6;
    }
    if (!(csts_final & NVME_CSTS_RDY)) { kputs("[nvme] 使能后 RDY 未起\n"); return -5; }
    kputs("[nvme] 控制器就绪\n");

    /* Identify Controller（CNS=1）—— 复用 nv_buf（init 专用，运行期才做数据 DMA）
     *
     * ⚠️ PRP 规则（以 QEMU 8.2.2 hw/nvme/ctrl.c 的 nvme_map_prp() 为准）：
     *    trans_len = page_size - (prp1 % page_size);  trans_len = MIN(len, trans_len);
     *    map(prp1, trans_len);  len -= trans_len;  if (len) 才用 prp2/prp list。
     *    函数里的 num_prps = (len >> page_bits) + 1 **只传给 trace，不参与逻辑**。
     *    → 只要 PRP1 页对齐且 len <= page_size，len 一次就被吃完，PRP2 根本不会被访问。
     *    （上一版注释误以为「4096B 必须双 PRP」，是错的。）
     *
     *    但 PRP2 仍填一个**有效**的第二页地址（NV_BUF_OFF+4096 落在第 6 页内）：
     *    真实控制器若在边界情况下读它，不会踩到越界内存；QEMU 下它被忽略，无害。
     *    旧代码把 PRP2 指向 &nv_buf[4096]，而 dbuf 只有 4096B → 越界，已修。
     *
     * 命令字段布局（严格按 QEMU 的 NvmeCmd / Linux 的 nvme_common_command，
     * 两者逐字节一致）：
     *   cmd[0] = opcode | flags<<8 | cid<<16
     *   cmd[1] = nsid
     *   cmd[6] = PRP1 低 32 位, cmd[7] = PRP1 高 32 位
     *   cmd[8] = PRP2 低 32 位, cmd[9] = PRP2 高 32 位
     *   cmd[10]= CDW10（Identify 里是 CNS<<0）
     *   cmd[11]= CDW11
     *   cmd[12]= CDW12 / SLBA 低
     *   cmd[13]= SLBA 高
     */
    for (u32 i = 0; i < 4096; i++) nv_buf[i] = 0;
    nv_clean_buf(4096);
    {
        u32 cmd[16]; for (int i = 0; i < 16; i++) cmd[i] = 0;
        cmd[0] = 0x06;                        /* Identify */
        cmd[1] = 0xffffffffu;                 /* nsid（controller identify 忽略） */
        cmd[6] = (u32)((u64)&nv_buf[0]);      /* PRP1 低 */
        cmd[7] = (u32)((u64)&nv_buf[0] >> 32);/* PRP1 高 */
        cmd[8] = (u32)((u64)&nv_buf[4096]);   /* PRP2 低（第二页） */
        cmd[9] = (u32)((u64)&nv_buf[4096] >> 32);
        cmd[10] = 0x01;                       /* CNS=1：控制器结构 */
        int r = nv_admin(cmd);
        if (r != 0) { kputs("[nvme] Identify Controller 失败 st="); kputdec((u32)r); kputs("\n"); return -7; }
    }
    nv_inv_buf(4096);
    /* NN：命名空间数量，Identify Controller 结构偏移 516（小端 u32）。
     * ⚠️ 旧代码用 be32() 读 → 得到 0x01000000 之类的垃圾值。
     *    QEMU 里是 id->nn = cpu_to_le32(NVME_MAX_NAMESPACES)。 */
    u32 nn = le32(nv_buf + 516);
    kputs("[nvme] 命名空间数="); kputdec(nn); kputs("\n");
    if (!nn) { kputs("[nvme] 没有命名空间\n"); return -8; }
    nvme_nsid = 1;                            /* 单盘场景直接用 nsid=1 */

    /* Identify Namespace（CNS=0）—— 同样复用 nv_buf（上一份控制器数据已用毕） */
    for (u32 i = 0; i < 4096; i++) nv_buf[i] = 0;
    nv_clean_buf(4096);
    {
        u32 cmd[16]; for (int i = 0; i < 16; i++) cmd[i] = 0;
        cmd[0] = 0x06;
        cmd[1] = nvme_nsid;                   /* nsid */
        cmd[6] = (u32)((u64)&nv_buf[0]);  cmd[7] = (u32)((u64)&nv_buf[0] >> 32);
        cmd[8] = (u32)((u64)&nv_buf[4096]); cmd[9] = (u32)((u64)&nv_buf[4096] >> 32);
        cmd[10] = 0x00;                       /* CNS=0：命名空间结构 */
        int r = nv_admin(cmd);
        if (r != 0) { kputs("[nvme] Identify Namespace 失败 st="); kputdec((u32)r); kputs("\n"); return -9; }
    }
    nv_inv_buf(4096);
    /* NSZE：命名空间逻辑块总数，结构偏移 0（小端 u64） */
    u64 nsze = le64(nv_buf + 0);
    /* FLBAS：偏移 26 的低 4 位 = 当前 LBA 格式索引。
     * ⚠️ 旧代码错读 nv_buf[128]（那是 LBAF 数组起点，不是 FLBAS）。 */
    u8 flbas = nv_buf[26] & 0x0f;
    /* LBAF 数组：偏移 128，每项 4 字节（QEMU 的 NvmeLBAF / Linux 的 nvme_lbaf 一致）：
     *   byte0-1 = MS（元数据大小）
     *   byte2   = DS = LBADS（log2 逻辑块字节数）
     *   byte3   = RP（相对性能）
     * ⚠️ 旧代码把整个 LBAF 当成一个大端 u32 再取 bit[23:16]，那取到的是 MS，不是 LBADS。
     *    正确做法：直接取该 4 字节项的第 3 个字节（索引 2）。 */
    u8 lbads = nv_buf[128 + 4 * flbas + 2];
    if (lbads < 9) lbads = 9;                 /* 至少 512 字节 */
    nvme_block = (1u << lbads);
    nvme_cap_sec = nsze * ((u64)nvme_block / 512);
    kputs("[nvme] 块大小="); kputdec(nvme_block);
    kputs(" flbas="); kputdec((u32)flbas);
    kputs(" 容量(512B扇区)="); kputdec((u32)(nvme_cap_sec >> 32));
    kputs(" "); kputdec((u32)nvme_cap_sec); kputs("\n");

    /* 创建 IO 完成队列（qid=1） */
    {
        u32 cmd[16]; for (int i = 0; i < 16; i++) cmd[i] = 0;
        cmd[0] = 0x05;                        /* Create I/O Completion Queue */
        cmd[6] = (u32)((u64)io_cq); cmd[7] = (u32)((u64)io_cq >> 32);
        cmd[10] = ((IO_DEPTH - 1) << 16) | 1;  /* [15:0]=QID=1, [31:16]=QSIZE-1 */
        cmd[11] = 0x01;                        /* [0]=PC(物理连续) */
        if (nv_admin(cmd) != 0) { kputs("[nvme] 创建 IO CQ 失败\n"); return -11; }
    }
    /* 创建 IO 提交队列（qid=1，绑到 CQ1） */
    {
        u32 cmd[16]; for (int i = 0; i < 16; i++) cmd[i] = 0;
        cmd[0] = 0x01;                         /* Create I/O Submission Queue */
        cmd[6] = (u32)((u64)io_sq); cmd[7] = (u32)((u64)io_sq >> 32);
        cmd[10] = ((IO_DEPTH - 1) << 16) | 1;   /* QID=1, QSIZE-1 */
        cmd[11] = (1u << 16) | 0x01;            /* [31:16]=CQID=1, [0]=PC */
        if (nv_admin(cmd) != 0) { kputs("[nvme] 创建 IO SQ 失败\n"); return -12; }
    }

    nvme_ok = 1;
    kputs("[nvme] 就绪\n");
    return 0;
}

int nv_present(void) { return nvme_ok; }
u64 nv_capacity_bytes(void) { return nvme_ok ? nvme_cap_sec * 512ull : 0; }
static u64 nv_capacity_sectors_impl(void) { return nv_capacity_bytes() / 512ull; }

/* ---- 注册到统一块设备层（blkdev）---- */
const struct blkdev_ops nv_ops = {
    .name             = "nvme",
    .init             = nv_init,
    .present          = nv_present,
    .capacity_bytes   = nv_capacity_bytes,
    .capacity_sectors = nv_capacity_sectors_impl,
    .read             = nv_read,
    .write            = nv_write,
};

/* ---------------- 读写（调用方传 512B 扇区；内部按块大小换算并分批） ---------------- */
int nv_read(u64 lba, void *buf, u32 nsec)
{
    if (!nvme_ok) return -1;
    u32 done = 0;
    while (done < nsec) {
        u32 chunk_sec = (nsec - done > 8) ? 8 : (nsec - done);   /* ≤4KB（=8×512）单次 PRP */
        u64 lba_blk = ((u64)(lba + done) * 512ull) / nvme_block;  /* 512B 扇区号 → NVMe 逻辑块号 */
        u32 nblk = (u32)(((u64)chunk_sec * 512ull) / nvme_block); /* 本次读多少逻辑块 */
        for (u32 i = 0; i < chunk_sec * 512; i++) nv_buf[i] = 0;
        nv_clean_buf(4096);
        {
            u32 cmd[16]; for (int i = 0; i < 16; i++) cmd[i] = 0;
            cmd[0] = 0x02;                          /* Read */
            cmd[1] = nvme_nsid;
            cmd[6] = (u32)((u64)nv_buf); cmd[7] = (u32)((u64)nv_buf >> 32);
            cmd[10] = (u32)(lba_blk & 0xffffffffu);
            cmd[11] = (u32)(lba_blk >> 32);
            cmd[12] = nblk - 1;                      /* 逻辑块数 - 1 */
            if (nv_io(cmd) != 0) return -3;
        }
        nv_inv_buf(4096);
        u8 *d = (u8 *)buf + (u64)done * 512;
        for (u32 i = 0; i < chunk_sec * 512; i++) d[i] = nv_buf[i];
        done += chunk_sec;
    }
    return 0;
}

int nv_write(u64 lba, const void *buf, u32 nsec)
{
    if (!nvme_ok) return -1;
    u32 done = 0;
    while (done < nsec) {
        u32 chunk_sec = (nsec - done > 8) ? 8 : (nsec - done);
        u64 lba_blk = ((u64)(lba + done) * 512ull) / nvme_block;
        u32 nblk = (u32)(((u64)chunk_sec * 512ull) / nvme_block);
        const u8 *s = (const u8 *)buf + (u64)done * 512;
        for (u32 i = 0; i < chunk_sec * 512; i++) nv_buf[i] = s[i];
        nv_clean_buf(4096);
        {
            u32 cmd[16]; for (int i = 0; i < 16; i++) cmd[i] = 0;
            cmd[0] = 0x01;                          /* Write */
            cmd[1] = nvme_nsid;
            cmd[6] = (u32)((u64)nv_buf); cmd[7] = (u32)((u64)nv_buf >> 32);
            cmd[10] = (u32)(lba_blk & 0xffffffffu);
            cmd[11] = (u32)(lba_blk >> 32);
            cmd[12] = nblk - 1;
            if (nv_io(cmd) != 0) return -3;
        }
        nv_inv_buf(4096);
        done += chunk_sec;
    }
    return 0;
}
