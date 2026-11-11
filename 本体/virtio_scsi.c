#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
#include "blkdev.h"    /* 块设备统一抽象层：导出 ops */
/* LoongOS 阶段6③ —— virtio-scsi 磁盘驱动（modern virtio-pci，VIRTIO 1.0）
 *
 * 为什么需要它（2026-10-25）：
 *   飞牛/libvirt 的虚拟机默认把硬盘挂成 `bus='scsi'`，而 QEMU 的 virtio-scsi 控制器
 *   是**半虚拟化设备**（1af4:1008/1048），不是真 SCSI 硬件。virtio-blk 只认 1af4:1001/1042，
 *   所以在 SCSI 虚拟机里「认不到盘」，只能退化成内存盘（文件不持久）。
 *   与其让用户改虚拟机配置，不如让系统适配环境 —— 本文件就是补上这个设备。
 *
 * 与 virtio-blk 的差别（只在「命令层」，传输层完全一样）：
 *   virtio-blk 直接用 {type, sector, ...} 头描述读/写；
 *   virtio-scsi 要发**标准 SCSI CDB**：请求 = {lun, tag, cdb[32]} → 可选数据段 → {status, residual, sense}
 *   队列也要注意：**queue 0 是控制队列**，请求队列从 **queue 1** 开始。
 *
 * 只用最小命令集（足够跑文件系统）：
 *   READ CAPACITY(10)  0x25  → 8 字节：末 LBA(BE32) + 块大小(BE32)
 *   READ(10)           0x28
 *   WRITE(10)          0x2A
 *
 * ⚠️ MMIO 全部走 rd8/.../wr64 单次内联访问（同 virtio_blk.c 的说明：强度削减产生的
 *    后索引 ldr/str 在 KVM stage-2 下会触发 ISV=0 的 data abort）。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;


/* ---------------- PCI ---------------- */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_MMIO_WINDOW 0x10000000UL
#define PCI_VENDOR      0x00
#define PCI_BAR0        0x10
#define PCI_CAP_PTR     0x34
#define PCI_CMD         0x04
#define PCI_CMD_MSE     (1u << 1)
#define PCI_CMD_BME     (1u << 2)

#define VIRTIO_VENDOR        0x1AF4
/* QEMU 8.2.2 hw/virtio/virtio-pci.c：transitional 用 pci_ids.h 里的
   PCI_DEVICE_ID_VIRTIO_SCSI，modern 用 0x1040 + VIRTIO_ID_SCSI(=8)。
   部分固件/libvirt 组合还会报 0x1008（按 virtio device id 编号的 transitional），
   一并认下，避免「设备明明在却因 ID 不匹配而 present=0」。 */
#define VIRTIO_SCSI_TRANS    0x1004      /* transitional（QEMU pci_ids.h） */
#define VIRTIO_SCSI_TRANS2   0x1008      /* transitional（virtio device id 编号） */
#define VIRTIO_SCSI_MODERN   0x1048      /* modern only */

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

struct vdesc { u64 addr; u32 len; u16 flags; u16 next; };
struct vused_elem { u32 id; u32 len; };

/* virtio-scsi 请求/响应（VIRTIO 1.0 §5.6.6） */
struct vs_req {
    u8  lun[8];          /* 简单寻址：[0]=0, [1]=target, [2..3]=lun */
    u64 id;              /* 任务标签，device 原样回传 */
    u8  task_attr;       /* 0 = simple */
    u8  prio;
    u8  crn;
    u8  cdb[32];
};
struct vs_resp {
    u32 sense_len;
    u32 residual;
    u16 status_qualifier;
    u8  status;          /* SCSI 状态：0 = GOOD */
    u8  response;        /* virtio-scsi 响应：0 = OK */
    u8  sense[96];
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
static inline void sbar(void)         { __asm__ volatile("dsb sy" ::: "memory"); }

/* ---------------- 状态 ---------------- */
static int  vs_ok;
static u64  vs_common, vs_notify, vs_isr, vs_devcfg;
static u32  vs_notify_mul;
static u64  vs_bars_dbg[6];
int vs_dbg_bdf = -1;
u32 vs_dbg_id;
u32 vs_dbg_raw[6];
static u64  vs_cap_sec;              /* 容量（512B 扇区） */
static u16  vs_qnum = 8;
static u16  vs_notify_off;
/* ⚠️ virtio-scsi 的队列布局：0=控制队列，1=**事件队列**，2..=请求队列。
   （早期误以为请求队列从 1 开始 —— 那条其实是事件队列，发请求永远没人处理。
   2026-10-25 用 QMP x-query-virtio-queue-status 才看清：q2 的 vring 被 UEFI 固件用过。） */
static u16  vs_qsel = 2;
static u32  vs_last_status;          /* 最近一次 SCSI 状态（调试用） */
static u8   vs_lun[8];               /* 目标 LUN（SAM 简单寻址：lun[1]=target, lun[2..3]=lun） */
static int  vs_tgt, vs_lunid;

/* 同步请求一次只用 3 个描述符，8 条足够；数组越小 BSS 越省（BSS 撞用户区会停机） */
static struct vdesc vs_desc[128] __attribute__((aligned(4096), section(".bss.align4k")));  /* 与 virtio_blk.c 同规格：4KB 对齐、足量槽位 */
static struct {
    u16 flags, idx;
    u16 ring[128];
    u16 used_event;
} vs_avail __attribute__((aligned(4096), section(".bss.align4k")));
static struct {
    u16 flags, idx;
    struct vused_elem ring[128];
    u16 avail_event;
} vs_used __attribute__((aligned(4096), section(".bss.align4k")));

static struct vs_req  vs_reqbuf __attribute__((aligned(64)));
static struct vs_resp vs_respbuf __attribute__((aligned(64)));
static u8 vs_iobuf[4096] __attribute__((aligned(64)));      /* DMA 缓冲，64B 缓存行对齐 */

/* ---------------- PCI 小工具（与 virtio_blk.c 同款） ---------------- */
static volatile u32 *ecam(int bus, int dev, int fn, int reg)
{
    return (volatile u32 *)(PCI_ECAM_BASE + ((u64)bus << 20) + ((u64)dev << 15)
                            + ((u64)fn << 12) + (reg & 0xffc));
}
static u32 cfg_rd(int b, int d, int f, int r) { return *ecam(b, d, f, r); }
static void cfg_wr(int b, int d, int f, int r, u32 v) { *ecam(b, d, f, r) = v; sbar(); }

/* （2026-10-28 清理）bar_read/bar_size/bar_assign 已删——编译报"定义未使用"的死代码：
   现代模式直接从 PCI 配置空间读 BAR（见 vs_init 里 rd64(rd_base + QUEUE_DESC)），
   legacy 模式用固件分配好的地址。历史教训留档：PCIe 拓扑下绝不自己挪 BAR——
   root port 的 window 覆盖不到，MMIO 全读回 0xFFFF（2026-10-25 实测，reset 轮询超时）。 */

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static void vs_notify_queue(void)
{
    /* ⚠️ 这里写的是**队列索引**：virtio-scsi 的队列 0 是控制队列，
       请求队列是 1。写 0 会把通知发给控制队列 → 请求永远没人处理（2026-10-25 踩过）。 */
    u64 a = vs_notify + (u64)vs_notify_off * (u64)(vs_notify_mul ? vs_notify_mul : 1);
    wr16(a, vs_qsel);
    sbar();
}

/* ================= legacy virtio-pci 回退（照 U-Boot drivers/virtio/virtio_pci_legacy.c） =================
   为什么需要：飞牛虚拟机里 virtio-scsi 挂在 PCIe 根端口后，固件把现代接口的 64 位 BAR4
   分到 0x40_10300000，我们的内核访问它直接 external abort（FAR_EL1=0x4010300010）。
   但这台设备是 transitional(1af4:1004)，**legacy 接口在 BAR1（32 位内存 BAR）**，
   走 legacy 完全不需要碰那个 64 位 BAR。legacy 寄存器（相对 BAR1）：
     0x00 host_features(u32) 0x04 guest_features(u32) 0x08 queue_pfn(u32)
     0x0c queue_num(u16)     0x0e queue_sel(u16)      0x10 queue_notify(u16)
     0x12 status(u8)         0x13 isr(u8)
   legacy 的队列必须是**一块连续内存**：desc 表 → avail → (补齐到 4KB) → used。 */
#define L_HOST_FEAT    0x00
#define L_GUEST_FEAT   0x04
#define L_QUEUE_PFN    0x08
#define L_QUEUE_NUM    0x0c
#define L_QUEUE_SEL    0x0e
#define L_QUEUE_NOTIFY 0x10
#define L_STATUS       0x12
#define L_ISR          0x13
#define L_CFG          0x14

static u64 vs_lbase;                 /* BAR1 基址 */
static int vs_legacy;
static u16 vs_lqnum;
static u8  vs_lq[8192] __attribute__((aligned(4096), section(".bss.align4k")));
static struct vdesc  *vs_ldesc;
static volatile u16  *vs_lavail_idx;
static volatile u16  *vs_lused_idx;

static int vs_legacy_init(int bus, int dev, int fn)
{
    /* ⚠️ legacy 接口仅在「BAR0 是 32 位内存 BAR」时才被调用（调用方已判定）。
       AArch64 上 QEMU 的 virtio-pci 把 legacy 放在 BAR0 的 I/O 端口条，端口 I/O 不可用，
       所以本架构下 legacy 基本不走；保留此路径仅作兜底。基址一律取 BAR0。 */
    u32 bar0 = cfg_rd(bus, dev, fn, PCI_BAR0);
    if (bar0 & 1u) return -1;                 /* 不是内存 BAR（I/O 条不可用） */
    u64 base = (u64)(bar0 & ~0xfu);
    if (!base) return -2;
    /* 自行开启 MSE(内存空间)+BME(总线主控/DMA)：legacy 路径必须自己开，不能依赖 UEFI 残留。 */
    {
        u32 cmd = cfg_rd(bus, dev, fn, PCI_CMD);
        cfg_wr(bus, dev, fn, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);
    }
    wr8(base + L_STATUS, 0); sbar();
    /* legacy 复位异步：等到 status 清 0 再继续 */
    { int t = 0; u8 s; do { s = rd8(base + L_STATUS); t++; } while (s != 0 && t < 1000000); }
    wr8(base + L_STATUS, 1); sbar();          /* ACK */
    wr8(base + L_STATUS, 3); sbar();          /* ACK|DRIVER */
    wr32(base + L_GUEST_FEAT, 0);             /* 不要任何特性（legacy 最小集） */
    wr16(base + L_QUEUE_SEL, 0);
    sbar();
    u16 num = rd16(base + L_QUEUE_NUM);
    if (!num) return -3;
    /* ⚠️ legacy 协议没有「设置队列大小」：设备用自己报告的 QUEUE_NUM 解析整个 virtqueue 布局
       （描述符表/可用环/已用环偏移都依赖 N）。必须用设备真实深度，绝不能截断：否则可用环偏移
       与设备解析错位 → 设备永远看不到新缓冲 → 命令永不完成（r=4 超时）。 */
    if (num > 128) return -3;                 /* 超出缓冲/数组能力（vs_lq 装得下 128） */
    for (u32 i = 0; i < sizeof(vs_lq); i++) vs_lq[i] = 0;
    u16 qn = num;                             /* 用设备真实深度，保证与设备布局一致 */
    vs_ldesc      = (struct vdesc *)vs_lq;
    vs_lavail_idx = (volatile u16 *)(vs_lq + (u32)qn * 16 + 2);
    vs_lused_idx  = (volatile u16 *)(vs_lq + 4096 + 2);
    wr32(base + L_QUEUE_PFN, (u32)((u64)vs_lq >> 12));
    sbar();
    if (!rd32(base + L_QUEUE_PFN)) return -4;
    wr8(base + L_STATUS, 3 | 4);              /* DRIVER_OK */
    sbar();
    if (rd8(base + L_STATUS) == 0) return -5;
    vs_lbase = base; vs_lqnum = qn; vs_legacy = 1;
    kputs("[scsi] legacy 模式就绪（BAR0="); kputhex(base);
    kputs(" qn="); kputdec(qn); kputs("）\n");
    return 0;
}


/* 发一条 SCSI 命令。data/data_len 可为空；for_write=1 表示数据方向是「写进设备」 */
static int vs_cmd_once(const u8 *cdb, void *data, u32 data_len, int for_write, u32 *out_val)
{
    if (!vs_ok) return -1;

    for (u32 i = 0; i < sizeof(vs_reqbuf); i++) ((u8 *)&vs_reqbuf)[i] = 0;
    for (u32 i = 0; i < 8; i++) vs_reqbuf.lun[i] = vs_lun[i];
    vs_reqbuf.id = 0x4c4f4f53u;                            /* "LOOS"，纯标记 */
    vs_reqbuf.task_attr = 0;
    for (u32 i = 0; i < 32; i++) vs_reqbuf.cdb[i] = cdb[i];
    vs_respbuf.status = 0xff;
    vs_respbuf.response = 0xff;
    vs_respbuf.residual = 0;
    sbar();

    /* 描述符顺序（照 Linux drivers/scsi/virtio_scsi.c 的 __virtscsi_add_cmd）：
         读命令：[请求(可读)] [响应(可写)] [数据(可写)]
         写命令：[请求(可读)] [数据(可读)] [响应(可写)]
       ⚠️ 两条硬规矩，2026-10-25 都被实机逮到过：
       1) VIRTIO 要求「所有可读描述符在前、可写在后」，写命令把数据放响应后面 →
          QEMU 直接报 "Incorrect order for descriptors" 并把设备判死；
       2) VIRTIO 1.0 未协商 ANY_LAYOUT 时，QEMU 把第一个可写描述符当响应头 →
          读命令的响应必须在数据之前，且响应描述符必须带 NEXT 位把环链接下去。 */
    struct vdesc *D = vs_legacy ? vs_ldesc : vs_desc;
    u32 nd = 0;
    D[nd].addr = (u64)&vs_reqbuf;
    D[nd].len  = (u32)sizeof(struct vs_req);
    D[nd].flags = VDESC_NEXT;
    D[nd].next = (u16)(nd + 1);
    nd++;
    if (data && data_len && for_write) {          /* 写命令：数据段插在响应之前 */
        D[nd].addr = (u64)data;
        D[nd].len  = data_len;
        D[nd].flags = VDESC_NEXT;           /* 设备读，无 WRITE 位 */
        D[nd].next = (u16)(nd + 1);
        nd++;
    }
    D[nd].addr = (u64)&vs_respbuf;
    D[nd].len  = (u32)sizeof(struct vs_resp);
    /* 读命令时响应后面还挂着数据段 → 必须带 NEXT；写命令时响应就是最后一环。 */
    D[nd].flags = VDESC_WRITE | ((data && data_len && !for_write) ? VDESC_NEXT : 0u);
    D[nd].next = (u16)(nd + 1);
    nd++;
    if (data && data_len && !for_write) {         /* 读命令：数据段在响应之后 */
        D[nd].addr = (u64)data;
        D[nd].len  = data_len;
        D[nd].flags = VDESC_WRITE;
        D[nd].next = 0;
        nd++;
    } else {
        D[nd - 1].flags = VDESC_WRITE;            /* 响应是最后一环（D = 当前模式的队列，legacy/modern 都对） */
        D[nd - 1].next = 0;
    }
    sbar();

    u16 qn = vs_legacy ? vs_lqnum : vs_qnum;
    u16 slot = vs_legacy ? (*vs_lavail_idx % qn) : (vs_avail.idx % qn);
    u16 before = vs_legacy ? *vs_lused_idx : vs_used.idx;   /* 快照必须在通知之前 */
    if (vs_legacy) {
        volatile u16 *ring = (volatile u16 *)((u8 *)vs_lavail_idx + 2);
        ring[slot] = 0;
        sbar();
        (*vs_lavail_idx)++;
        sbar();
        wr16(vs_lbase + L_QUEUE_NOTIFY, 0);
        sbar();
    } else {
        vs_avail.ring[slot] = 0;
        sbar();
        vs_avail.idx++;
        sbar();
        vs_notify_queue();
    }
    for (u64 spin = 0; spin < 60000000ull; spin++) {
        sbar();
        u16 now = vs_legacy ? *vs_lused_idx : vs_used.idx;
        if (now != before) {
            if (vs_legacy) (void)rd8(vs_lbase + L_ISR); else (void)rd8(vs_isr);
            vs_last_status = vs_respbuf.status;
            if (vs_respbuf.status != 0 || vs_respbuf.response != 0) return -3;
            if (out_val && data && data_len >= 4) *out_val = be32((const u8 *)data);
            return 0;
        }
    }
    return -4;                       /* 超时 */
}

/* 带一次重试的封装：设备刚上电时常回 CHECK CONDITION（UNIT ATTENTION，"power on reset"），
   这是 SCSI 正常行为 —— 发 REQUEST SENSE 把条件清掉，再重发原命令即可。
   ⚠️ 2026-10-25 教训：早先靠一条 TUR 顺手清掉，清理调试代码时删掉 TUR → 全盘识别失败（回归）。 */
static int vs_cmd(const u8 *cdb, void *data, u32 data_len, int for_write, u32 *out_val)
{
    int r = vs_cmd_once(cdb, data, data_len, for_write, out_val);
    if (r == -3 && vs_respbuf.status != 0) {
        u8 rs[16];
        for (int i = 0; i < 16; i++) rs[i] = 0;
        rs[0] = 0x03;                      /* REQUEST SENSE */
        rs[4] = 18;
        u8 sense[32];
        for (u32 i = 0; i < 32; i++) sense[i] = 0;
        if (vs_cmd_once(rs, sense, 18, 0, 0) == 0)
            r = vs_cmd_once(cdb, data, data_len, for_write, out_val);
    }
    return r;
}


/* ---------------- 初始化 ---------------- */
int vs_init(void)
{
    pci_enum();                     /* 确保总线号/BAR/桥窗口已就绪（固件没配则自举分配） */
    vs_ok = 0;
    /* ⚠️ 多级 PCIe 桥遍历（2026-11-08 修复真机找不到盘的根因）：
       飞牛/libvirt 的虚拟机里 virtio-scsi 控制器挂在根端口 → 交换机 → 下行端口之后，
       控制器实际在 secondary bus 4（BDF 形如 03:00.0 的桥把下游引到 bus 4）。
       只扫 bus0 上的一级桥、取它们的 secondary 会**漏掉多级桥链** —— 早先实现正是卡在这，
       真机「扫描总线数=2」却始终找不到 1af4:1048。

       正确做法：从 bus0 起步做 BFS，每发现一条桥就把它下游的 secondary 总线入队，
       直到没有新总线；再逐总线扫 virtio-scsi 设备。visited[] 保证不重复、不超过 ECAM 上限，
       绝不盲扫不存在的总线（否则 external abort 被自愈吞掉 → 屏幕反复刷异常）。
       思路同 U-Boot drivers/pci/pci-uclass.c 的 pci_get_next_bus() 递归枚举，
       以及 Linux drivers/pci/probe.c 的 pci_scan_child_bus()。 */
    int buses[64];
    static int visited[256];
    static int vs_next_bus;
    vs_next_bus = 1;
    for (int i = 0; i < 256; i++) visited[i] = 0;
    int head = 0, tail = 0;
    buses[tail++] = 0; visited[0] = 1;
    while (head < tail) {
        int b = buses[head++];
        for (int d = 0; d < 32; d++) {
            u32 v = cfg_rd(b, d, 0, PCI_VENDOR);
            if ((v & 0xffff) == 0xffff) continue;
            u32 cls = cfg_rd(b, d, 0, 0x08) >> 24;          /* base class: 0x06 = bridge */
            if (cls != 0x06) continue;                       /* PCI/PCIe 桥（含 root port、switch） */
            u32 sec = (cfg_rd(b, d, 0, 0x18) >> 8) & 0xff;   /* secondary bus number */
            /* ⚠️ 固件没分配总线号（裸 ELF 直启 / 部分固件）时自己分配：
               不写 secondary 桥就不转发该总线的配置周期，下游设备永远不可达。
               已分配（UEFI 启动）则原样保留，绝不乱改。 */
            if (!sec) {
                sec = (u32)vs_next_bus++;
                if (sec < 0x100)
                    cfg_wr(b, d, 0, 0x18,
                           ((u32)b & 0xffu) | ((u32)sec << 8) | (0xFFu << 16));  /* primary=b, secondary=sec, subordinate=0xFF */
            }
            if (sec >= 0x100) continue;
            if (!visited[sec] && tail < 64) { visited[sec] = 1; buses[tail++] = (int)sec; }
        }
    }
    int nb = tail;
    kputs("[scsi] 扫描总线数="); kputdec((u32)nb); kputs("\n");

    int bus = -1, dev = -1, fn = -1;
    for (int bi = 0; bi < nb && bus < 0; bi++)
        for (int d = 0; d < 32 && bus < 0; d++)
            for (int f = 0; f < 8; f++) {
                int b = buses[bi];
                u32 id = cfg_rd(b, d, f, PCI_VENDOR);
                if ((id & 0xffff) != VIRTIO_VENDOR) continue;
                u32 did = (id >> 16) & 0xffff;
                if (did != VIRTIO_SCSI_TRANS && did != VIRTIO_SCSI_TRANS2 &&
                    did != VIRTIO_SCSI_MODERN) continue;
                bus = b; dev = d; fn = f;
                break;
            }
    if (bus < 0) {
        kputs("[scsi] 没找到 virtio-scsi 设备(1af4:1004/1008/1048)\n");
        /* 失败时把所有已发现设备打出来：真机上用户敲 `scsi` 就能看到
           到底是「设备根本没枚举到」还是「ID/桥窗口不对」，不用再猜。
           图形终端会捕获这些输出。 */
        kputs("[scsi] PCI 清单 b:d.f id=vendor:device cls=baseclass\n");
        for (int bi = 0; bi < nb; bi++)
            for (int d = 0; d < 32; d++)
                for (int f = 0; f < 8; f++) {
                    int b = buses[bi];
                    u32 v = cfg_rd(b, d, f, PCI_VENDOR);
                    if ((v & 0xffff) == 0xffff) continue;
                    u32 cl = cfg_rd(b, d, f, 0x08);
                    kputs("[scsi]  "); kputdec((u32)b); kputs(":");
                    kputdec((u32)d); kputs("."); kputdec((u32)f);
                    kputs(" id="); kputhex((u64)v);
                    kputs(" cls="); kputhex((u64)(cl >> 24));
                    kputs("\n");
                }
        return -1;
    }
    vs_dbg_bdf = bus * 10000 + dev * 100 + fn;
    vs_dbg_id = cfg_rd(bus, dev, fn, PCI_VENDOR);
    for (int i = 0; i < 6; i++) vs_dbg_raw[i] = cfg_rd(bus, dev, fn, PCI_BAR0 + i * 4);
    kputs("[scsi] 找到设备 bus="); kputdec((u32)bus);
    kputs(" dev="); kputdec((u32)dev); kputs(" fn="); kputdec((u32)fn); kputs("\n");

    /* ⚠️ 关键：**绝不改写 BAR**。
       2026-10-25 实测（用户 PCIe 虚拟机）：原实现用 bar_assign 把 BAR4 挪到 0x10210000，
       但 BAR4 是 64 位 BAR —— 只写低半、高半仍留 0x40，设备实际落在 0x40_10210000，
       与我们算的地址不一致 → 访问落到无人应答的地址，触发 external abort（FAR_EL1=0x4010300010）。
       正确做法：只读固件已分配好的值，64 位 BAR 按 bit[2:1]==0b10 合并 BARn/BARn+1。 */
    /* 接口选择（2026-11-08 实测纠正）：QEMU 的 virtio-pci transitional 把 **legacy 接口放在
       BAR0 的 I/O 端口 BAR**（实测 BAR0=0x1，bit0=1=IO）。AArch64 没有 x86 式 IN/OUT，端口 I/O
       不可用 → legacy 基本走不通。旧代码曾误用 BAR1(0x10080000 内存条)当 legacy 基址，读到巧合值
       qn=64「假成功」，命令永远超时。因此**优先走 modern 接口**（BAR4 64 位内存，落点 0x8000000000+，
       内核 MMU 已把该高位 PCIe 窗口按 Device 映射，可直访）。仅当 BAR0 是内存 BAR 才兜底试 legacy。 */
    {
        u32 bar0 = cfg_rd(bus, dev, fn, PCI_BAR0);
        int is_mem = !(bar0 & 1u);
        if (is_mem) {
            int lr = vs_legacy_init(bus, dev, fn);
            kputs("[scsi] legacy init r="); kputdec((u32)(lr < 0 ? -lr : lr)); kputs("\n");
            if (lr == 0) { vs_ok = 1; goto scan; }
        } else {
            kputs("[scsi] BAR0=IO 条（legacy 不可用于 AArch64）→ 走 modern\n");
        }
    }

    u64 bars[6];
    for (int i = 0; i < 6; i++) {
        u32 lo = cfg_rd(bus, dev, fn, PCI_BAR0 + i * 4);
        if (i > 0 && (cfg_rd(bus, dev, fn, PCI_BAR0 + (i - 1) * 4) & 0x6u) == 0x4u) {
            bars[i] = 0;                       /* 这是上一个 64 位 BAR 的高半槽 */
            continue;
        }
        if ((lo & 0x6u) == 0x4u) {             /* 64 位 BAR：合并高半 */
            u32 hi = cfg_rd(bus, dev, fn, PCI_BAR0 + (i + 1) * 4);
            bars[i] = ((u64)hi << 32) | (u64)(lo & 0xfffffff0u);
        } else {
            bars[i] = (u64)(lo & 0xfffffff0u);
        }
    }

    u32 cmd = cfg_rd(bus, dev, fn, PCI_CMD);
    cfg_wr(bus, dev, fn, PCI_CMD, cmd | PCI_CMD_MSE | PCI_CMD_BME);

    u32 st = cfg_rd(bus, dev, fn, 0x04) >> 16;
    if (!((st >> 4) & 1)) { kputs("[scsi] 无能力链(不支持 modern)\n"); return -2; }

    u32 cap = cfg_rd(bus, dev, fn, PCI_CAP_PTR) & 0xff;
    vs_common = vs_notify = vs_isr = vs_devcfg = 0;
    for (int guard = 0; guard < 48 && cap; guard++) {
        u32 d0 = cfg_rd(bus, dev, fn, (int)(cap & 0xfc));
        u8  id = (u8)(d0 & 0xff);
        u32 nxt = (d0 >> 8) & 0xff;
        u32 cfgtype = (d0 >> 24) & 0xff;
        u32 d1 = cfg_rd(bus, dev, fn, (int)((cap + 4) & 0xfc));
        u32 baridx = d1 & 0xff;
        u32 off = cfg_rd(bus, dev, fn, (int)((cap + 8) & 0xfc));
        if (id == 0x09) {
            u64 base = bars[baridx & 7];
            kputs("[scsi] cap type="); kputdec(cfgtype);
            kputs(" bar="); kputdec(baridx);
            kputs(" off="); kputhex((u64)off);
            kputs(" base="); kputhex(base);
            kputs("\n");
            if (base) {
                if (cfgtype == VIRTIO_PCI_CAP_COMMON_CFG)      vs_common = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    vs_notify = base + off;
                    vs_notify_mul = cfg_rd(bus, dev, fn, (int)((cap + 16) & 0xfc));
                }
                else if (cfgtype == VIRTIO_PCI_CAP_ISR_CFG)    vs_isr = base + off;
                else if (cfgtype == VIRTIO_PCI_CAP_DEVICE_CFG) vs_devcfg = base + off;
            }
        }
        cap = nxt;
    }
    if (!vs_common || !vs_notify || !vs_devcfg) {
        kputs("[scsi] 缺配置区 common="); kputhex(vs_common);
        kputs(" notify="); kputhex(vs_notify);
        kputs(" devcfg="); kputhex(vs_devcfg); kputs("\n");
        return -3;
    }

    /* ⚠️ 安全闸：算出来的 MMIO 地址必须落在已知窗口内，否则宁可报错也不要摸
       （摸到不存在的地址会 external abort，被自愈吞掉 → 屏幕每几秒刷异常，非常难查）。
       已知窗口：32 位 MMIO 0x10000000-0x3effffff；高位 0x8000000000 以上。 */
    {
        u64 c = vs_common, n = vs_notify, d = vs_devcfg;
        int okc = (c >= 0x10000000ull && c < 0x3f000000ull) || (c >= 0x8000000000ull);
        int okn = (n >= 0x10000000ull && n < 0x3f000000ull) || (n >= 0x8000000000ull);
        int okd = (d >= 0x10000000ull && d < 0x3f000000ull) || (d >= 0x8000000000ull);
        if (!okc || !okn || !okd) {
            kputs("[scsi] 拒绝访问可疑 MMIO：common="); kputhex(c);
            kputs(" notify="); kputhex(n);
            kputs(" devcfg="); kputhex(d);
            kputs("\n");
            return -10;
        }
    }

    /* 复位 + 特征协商（只要 VIRTIO 1.0） */
    wr8(vs_common + CC_DEV_STATUS, 0);
    sbar();
    /* ⚠️ 复位是**异步**的：规范要求等到读回 0 才能继续（最多 1 秒）。
       不等待的话，固件(EDK2)启动时用过的设备可能还在收尾 → 后续配队列被判非法 → 设备 FAILED。
       2026-10-25 在用户的 PCIe 拓扑虚拟机上实测踩到（rc=7）。 */
    {
        int t = 0;
        while (rd8(vs_common + CC_DEV_STATUS) != 0 && t < 1000000) t++;
        kputs("[scsi] reset-wait loops="); kputdec((u32)t); kputs("\n");
    }
    wr8(vs_common + CC_DEV_STATUS, VST_ACK);
    wr8(vs_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER);
    wr32(vs_common + CC_DEV_FEAT_SEL, 0);
    u32 feat_lo = rd32(vs_common + CC_DEV_FEAT);
    wr32(vs_common + CC_DEV_FEAT_SEL, 1);
    u32 feat_hi = rd32(vs_common + CC_DEV_FEAT);
    u64 devf = ((u64)feat_hi << 32) | feat_lo;
    u64 want = 1ull << VIRTIO_F_VERSION_1;
    if (!(devf & want)) { kputs("[scsi] 不支持 VIRTIO1.0 feat="); kputhex(devf); kputs("\n"); return -4; }
    wr32(vs_common + CC_DRV_FEAT_SEL, 0); wr32(vs_common + CC_DRV_FEAT, (u32)want);
    wr32(vs_common + CC_DRV_FEAT_SEL, 1); wr32(vs_common + CC_DRV_FEAT, (u32)(want >> 32));
    kputs("[scsi] feat ok, 配队列\n");
    wr8(vs_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK);
    if (!(rd8(vs_common + CC_DEV_STATUS) & VST_FEATURES_OK)) { kputs("[scsi] FEATURES_OK 被拒\n"); return -5; }

    /* 队列：0 = 控制队列，**请求队列从 1 开始**（不合规的设备可能把请求队列放 0，兜底试一次） */
    u16 qsz = 0;
    vs_qsel = 2;      /* 第一条请求队列（0=控制, 1=事件） */
    wr16(vs_common + CC_QUEUE_SEL, vs_qsel);
    qsz = rd16(vs_common + CC_QUEUE_SIZE);
    if (!qsz) {
        /* 兜底：有的实现把请求队列放 1 */
        vs_qsel = 1;
        wr16(vs_common + CC_QUEUE_SEL, vs_qsel);
        qsz = rd16(vs_common + CC_QUEUE_SIZE);
        if (!qsz) { kputs("[scsi] 找不到请求队列\n"); return -6; }
    }
    if (!qsz) { kputs("[scsi] 队列不可用\n"); return -6; }
    if (qsz > 128) qsz = 128;
    vs_qnum = qsz;
    wr16(vs_common + CC_QUEUE_SIZE, qsz);
    for (u32 i = 0; i < sizeof(vs_desc); i++) ((u8 *)vs_desc)[i] = 0;   /* 全表清零 */
    for (u32 i = 0; i < sizeof(vs_avail); i++) ((u8 *)&vs_avail)[i] = 0;
    for (u32 i = 0; i < sizeof(vs_used); i++) ((u8 *)&vs_used)[i] = 0;
    wr64(vs_common + CC_QUEUE_DESC,   (u64)vs_desc);
    wr64(vs_common + CC_QUEUE_DRIVER, (u64)&vs_avail);
    wr64(vs_common + CC_QUEUE_DEVICE, (u64)&vs_used);
    wr16(vs_common + CC_QUEUE_ENABLE, 1);
    {
        u16 en = rd16(vs_common + CC_QUEUE_ENABLE);
        kputs("[scsi] queue_enable readback="); kputdec(en); kputs("\n");
        if (en != 1) {
            kputs("[scsi] rawBAR:");
            for (int i = 0; i < 6; i++) kputs(" "), kputhex((u64)cfg_rd(bus, dev, fn, PCI_BAR0 + i * 4));
            kputs("\n[scsi] cmd="); kputhex((u64)(cfg_rd(bus, dev, fn, PCI_CMD) & 0xffff));
            kputs(" common="); kputhex(vs_common);
            kputs("\n"); kputs("[scsi] 队列使能失败\n"); return -6;
        }
    }
    vs_notify_off = rd16(vs_common + CC_QUEUE_NOTIFY_OFF);


    wr8(vs_common + CC_DEV_STATUS, VST_ACK | VST_DRIVER | VST_FEATURES_OK | VST_DRIVER_OK);
    kputs("[scsi] rawBAR:");
    for (int i = 0; i < 6; i++) kputs(" "), kputhex((u64)cfg_rd(bus, dev, fn, PCI_BAR0 + i * 4));
    kputs("\n[scsi] BARs:");
    for (int i = 0; i < 6; i++) kputs(" "), kputhex(vs_bars_dbg[i]);
    kputs("\n[scsi] common="); kputhex(vs_common);
    kputs(" notify="); kputhex(vs_notify);
    kputs(" devcfg="); kputhex(vs_devcfg);
    kputs("\n");
    {
        u8 st2 = rd8(vs_common + CC_DEV_STATUS);
        kputs("[scsi] device_status="); kputdec(st2); kputs("\n");
        if (st2 & VST_FAILED) {
            kputs("[scsi] 设备 FAILED (qsz="); kputdec(vs_qnum);
            kputs(" off="); kputdec(vs_notify_off);
            kputs(" mul="); kputdec(vs_notify_mul); kputs(")\n");
            return -7;
        }
    }

    vs_ok = 1;


    /* 读容量：LUN 扫描结束后发 READ CAPACITY(10) → 末 LBA(4) + 块大小(4)
       （cdb 在循环里现构造；原先这里的 cdb 设置从没被用到 → 2026-10-28 清掉编译警告） */
    u32 last = 0;
scan:
    /* 扫 LUN。QEMU 的 virtio_scsi_device_get()（hw/scsi/virtio-scsi.c）规定：
         lun[0] 必须 == 1（简单寻址标记）
         lun[2] 必须 == 0 或落在 0x40..0x7f
         target = lun[1]，LUN = ((lun[2] << 8) | lun[3]) & 0x3FFF
       ⚠️ 2026-10-25 实测踩坑：lun[0] 写成 0 会被判「无此设备」→ 全部 BAD_TARGET，
          看起来像"QEMU 不认 SCSI 盘"，实际是编码错。
       飞牛/libvirt 布局：cdrom=LUN0、硬盘=LUN1 → 两个都探，再用 INQUIRY 的类型区分。 */
    {
        int found = 0, found_is_disk = 0;
        for (int t = 0; t < 2 && !found_is_disk; t++)
            for (int l = 0; l < 4 && !found_is_disk; l++) {
                for (int i = 0; i < 8; i++) vs_lun[i] = 0;
                vs_lun[0] = 1;
                vs_lun[1] = (u8)t;
                vs_lun[2] = (u8)((l >> 8) & 0x3f);
                vs_lun[3] = (u8)l;
                last = 0;
                u8 c2[16]; for (int i = 0; i < 16; i++) c2[i] = 0; c2[0] = 0x25;
                int pr0 = vs_cmd(c2, vs_iobuf, 8, 0, &last);
                if (pr0 != 0) {
                    kputs("[scsi] t="); kputdec((u32)t);
                    kputs(" l="); kputdec((u32)l);
                    kputs(" r="); kputdec((u32)(-pr0));
                    kputs(" resp="); kputdec(vs_respbuf.response);
                    kputs(" st="); kputdec(vs_respbuf.status);
                    kputs("\n");
                    continue;
                }
                u8 inq[36];
                for (int i = 0; i < 36; i++) inq[i] = 0;
                u8 c3[16]; for (int i = 0; i < 16; i++) c3[i] = 0;
                c3[0] = 0x12; c3[4] = 36;
                int is_disk = 0;
                if (vs_cmd(c3, inq, 36, 0, 0) == 0) is_disk = ((inq[0] & 0x1f) == 0x00);
                kputs("[scsi] 候选 target="); kputdec((u32)t);
                kputs(" lun="); kputdec((u32)l);
                kputs(" 类型="); kputdec((u32)(inq[0] & 0x1f));
                kputs(is_disk ? "（硬盘）" : "（非硬盘）");
                kputs(" 扇区数="); kputdec(last + 1); kputs("\n");
                if (is_disk || !found) { vs_tgt = t; vs_lunid = l; found = 1; }
                if (is_disk) found_is_disk = 1;
            }
        if (!found) { kputs("[scsi] 所有 LUN 都读不到容量\n"); vs_ok = 0; return -8; }
        kputs("[scsi] 使用 target="); kputdec((u32)vs_tgt);
        kputs(" lun="); kputdec((u32)vs_lunid); kputs("\n");
        u8 c4[16]; for (int i = 0; i < 16; i++) c4[i] = 0; c4[0] = 0x25;
        last = 0;
        if (vs_cmd(c4, vs_iobuf, 8, 0, &last) != 0) {
            kputs("[scsi] 重读容量失败\n"); vs_ok = 0; return -8;
        }
    }
    u32 bs = be32(vs_iobuf + 4);
    if (!bs) bs = 512;
    vs_cap_sec = ((u64)last + 1) * ((u64)bs / 512);
    if (!vs_cap_sec) { vs_ok = 0; return -9; }
    return 0;
}

int vs_present(void) { return vs_ok; }
u64 vs_capacity_bytes(void) { return vs_cap_sec * 512ull; }

/* ---------------- 读写（8 扇区一批，与 virtio-blk 语义一致） ---------------- */
int vs_read(u64 lba, void *buf, u32 nsec)
{
    if (!vs_ok) return -1;
    u32 done = 0;
    while (done < nsec) {
        u32 chunk = (nsec - done > 8) ? 8 : (nsec - done);
        u8 cdb[16];
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x28;                       /* READ(10) */
        put_be32(cdb + 2, (u32)(lba + done));
        cdb[7] = (u8)(chunk >> 8); cdb[8] = (u8)chunk;
        int r = vs_cmd(cdb, vs_iobuf, chunk * 512, 0, 0);
        if (r < 0) return r;
        u8 *d = (u8 *)buf + (u64)done * 512;
        if (d != vs_iobuf)
            for (u32 i = 0; i < chunk * 512; i++) d[i] = vs_iobuf[i];
        done += chunk;
    }
    return 0;
}

int vs_write(u64 lba, const void *buf, u32 nsec)
{
    if (!vs_ok) return -1;
    u32 done = 0;
    while (done < nsec) {
        u32 chunk = (nsec - done > 8) ? 8 : (nsec - done);
        const u8 *s = (const u8 *)buf + (u64)done * 512;
        if (s != vs_iobuf)
            for (u32 i = 0; i < chunk * 512; i++) vs_iobuf[i] = s[i];
        u8 cdb[16];
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x2a;                       /* WRITE(10) */
        put_be32(cdb + 2, (u32)(lba + done));
        cdb[7] = (u8)(chunk >> 8); cdb[8] = (u8)chunk;
        int r = vs_cmd(cdb, vs_iobuf, chunk * 512, 1, 0);
        if (r < 0) {
            kputs("[scsi] WRITE fail lba="); kputdec((u32)(lba + done));
            kputs(" n="); kputdec(chunk);
            kputs(" r="); kputdec((u32)(-r));
            kputs(" resp="); kputdec(vs_respbuf.response);
            kputs(" st="); kputdec(vs_respbuf.status);
            kputs("\n");
            return r;
        }
        done += chunk;
    }
    return 0;
}

u32 vs_status(void) { return vs_last_status; }

/* ---- 注册到统一块设备层（blkdev）----
 * 后续新增磁盘驱动只需照此导出一个 xxx_ops，不必改动 kernel.c / virtio_blk.c。 */
static u64 vs_dev_capacity_sectors(void) { return vs_capacity_bytes() / 512ull; }
const struct blkdev_ops vs_ops = {
    .name             = "virtio-scsi",
    .init             = vs_init,
    .present          = vs_present,
    .capacity_bytes   = vs_capacity_bytes,
    .capacity_sectors = vs_dev_capacity_sectors,
    .read             = vs_read,
    .write            = vs_write,
};
