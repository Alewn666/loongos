#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS 阶段6① / 桌面 P2 —— xHCI 主机控制器 + USB HID 键盘/鼠标/平板
 *
 * P2 目标（2026-11-08）：桌面要能用鼠标，且真机飞牛 VM 的拓扑是
 *   qemu-xhci 一个控制器 + 三个设备（usb-tablet / usb-mouse / usb-kbd，端口 1/2/3）。
 *   xHCI 一个 slot 只能绑定一个设备，所以本轮把「单 slot 单键盘」改成「多 slot 多设备」：
 *     - 每个设备独立的设备上下文 / EP0 环 / 中断端点环 / 报告缓冲；
 *     - 配置描述符按 QEMU 8.2.2 hw/usb/dev-hid.c 的实际字段识别三类接口；
 *     - 键盘 → kbd_feed(ASCII)，鼠标/平板 → kernel 的 mouse_feed_*；
 *     - Ctrl+Shift+F1 在报告层识别，直接请求桌面退出（F1 没有 ASCII）。
 *
 * 原有约束不变：无分配器（静态 64B 对齐 DMA）、无 MSI-X（纯轮询）、
 * 不依赖设备树、MMIO 一律走 rd32/wr32 内联汇编（KVM NISV 坑，见下方注释）。
 *
 * 参考：xHCI 1.1 规范 / Linux drivers/usb/host/xhci.h /
 *       QEMU 8.2.2 hw/usb/hcd-xhci.c + hw/usb/dev-hid.c + hw/input/hid.c
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---------- 内核侧接口（kernel.c 提供） ---------- */

/* 桌面 P2：鼠标事件与组合键（kernel.c 提供） */


/* ---------- 常量 ---------- */
/* QEMU virt（aarch64，highmem 默认开）实测内存映射（info mtree 真值）：
 *   PCIe 低 MMIO 窗口 0x10000000-0x3effffff（xHCI 的 32 位 BAR 落在这里）
 *   PCIe ECAM(MMCFG)  0x4010000000-0x401fffffff（256MB，bus 0-255）
 *   ⚠️ 别写成 0x3f000000 —— 那是老资料/x86 习惯，这个 QEMU 上那里根本没注册区域，
 *      访问会吃外部中止（实测 ESR EC=0x25 DFSC=0x10）。 */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_MMIO_BASE   0x10000000UL   /* virt 的 32 位 MMIO 窗口 */

/* PCI 配置空间寄存器 */
#define PCI_VENDOR_ID   0x00
#define PCI_CLASS       0x08           /* rev(8) | progif/subclass/class(24) */
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0        0x10
#define PCI_COMMAND     0x04
#define PCI_CMD_MSE     (1u << 1)      /* memory space enable */
#define PCI_CMD_BME     (1u << 2)      /* bus master enable */

/* xHCI 能力寄存器 */
#define XCAP_CAPLENGTH  0x00
#define XCAP_HCIVERSION 0x02
#define XCAP_HCSPARAMS1 0x04
#define XCAP_HCSPARAMS2 0x08
#define XCAP_HCCPARAMS1 0x10
#define XCAP_DBOFF      0x14
#define XCAP_RTSOFF     0x18

/* xHCI 操作寄存器（相对 oper base = BAR + CAPLENGTH） */
#define XOP_USBCMD      0x00
#define XOP_USBSTS      0x04
#define XOP_CRCR        0x18
#define XOP_DCBAAP      0x30
#define XOP_CONFIG      0x38
#define XOP_PORTSC(n)   (0x400 + (n - 1) * 0x10)   /* ⚠️ QEMU/规范都是 0-based：端口 n 在 0x400+(n-1)*0x10 */

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_INTE     (1u << 2)
#define USBSTS_HCH      (1u << 0)

/* PORTSC 位（QEMU hcd-xhci.c 90-120 行核对） */
#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PLS_SH   5
#define PORTSC_PLS_M    0xf
#define PORTSC_PP       (1u << 9)
#define PORTSC_SPEED_SH 10
#define PORTSC_SPEED_M  0xf
#define PORTSC_CSC      (1u << 17)
#define PORTSC_PEC      (1u << 18)
#define PORTSC_PRC      (1u << 21)

/* 中断器 0（runtime + 0x20） */
#define XRT_IMAN(n)     (0x20 + (n) * 0x20 + 0x00)
#define XRT_IMOD(n)     (0x20 + (n) * 0x20 + 0x04)
#define XRT_ERSTSZ(n)   (0x20 + (n) * 0x20 + 0x08)
#define XRT_ERSTBA(n)   (0x20 + (n) * 0x20 + 0x10)
#define XRT_ERDP(n)     (0x20 + (n) * 0x20 + 0x18)

/* TRB 通用位 */
#define TRB_C           (1u << 0)      /* cycle */
#define TRB_TYPE_SH     10
#define TRB_TYPE_M      0x3f
#define TRB_TYPE(t)     (((t) >> TRB_TYPE_SH) & TRB_TYPE_M)
#define TRB_SLOTID_SH   24
#define TRB_EPID_SH     16
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_CH          (1u << 4)
#define TRB_ISP         (1u << 2)
#define TRB_DIR_IN      (1u << 16)

/* TRB 类型 */
#define TR_NORMAL       1
#define TR_SETUP        2
#define TR_DATA         3
#define TR_STATUS       4
#define TR_LINK         6
#define CR_ENABLE_SLOT  9
#define CR_DISABLE_SLOT 10
#define CR_ADDRESS_DEV  11
#define CR_CONFIG_EP    12
#define CR_EVAL_CONTEXT 13
#define ER_TRANSFER     32
#define ER_CMD_COMPLETE 33
#define ER_PORT_STATUS  34

/* 完成码 */
#define CC_SUCCESS      1
#define CC_SHORT_PKT    13

/* 输入/设备上下文（CSZ=0 → 64B 上下文；见 spec 6.2） */
#define CTX_ADD_SLOT    (1u << 0)
#define CTX_ADD_EP0     (1u << 1)

/* 端点类型（ep_info2 bits 5:3） */
#define EP_TYPE_CTRL    4
#define EP_TYPE_INT_IN  7

struct trb { u64 param; u32 status; u32 control; };

/* ---------- 静态 DMA 结构（全部 64B 对齐） ---------- */
#define NCMD_TRB 32
#define NEVT_TRB 32
#define NEP0_TRB 16
#define NHID_TRB 8
#define MAX_HID  4                 /* 键盘 + 相对鼠标 + 绝对平板 + 预留一个 */

/* xHCI 一个 slot 一个设备上下文：CSZ=0 时是 32B slot + 31×32B EP = 1024B。
   多设备必须各自独立 —— DCBAA[slot] 指向各自的上下文，xHCI 在里面维护
   EP0 dequeue 指针；共享会让两个设备互相踩 TRB 与 cycle 状态。 */
struct hid_dev {
    int  slot;                  /* Enable Slot 返回的 slot id（1..MAX_HID） */
    int  port;
    int  speed;
    int  ep_num;                /* 中断 IN 端点号 */
    int  dci;                   /* 端点 DCI = 2*ep+1，doorbell 用 */
    u16  maxpkt;                /* 端点最大包长（键盘/平板 8，相对鼠标 4） */
    u8   interval;
    u8   kind;                  /* 1=键盘 2=鼠标(相对) 3=平板(绝对) */
    u8   ready;
    u8   prev_keys[6];          /* 上一份键盘报告的 6 个 keycode */
    u8   prev_mod;
    u8   prev_btn;
    int  enq;                   /* 中断环入队索引 */
    u32  cycle;                 /* 中断环 cycle */
    u32  ep0_enq;
    u32  ep0_cycle;
};

static volatile u64 dcbaa[64 + 1] __attribute__((aligned(64)));
static u8  dev_ctx[MAX_HID][1024] __attribute__((aligned(64)));
static u8  in_ctx[4096]          __attribute__((aligned(64)));   /* 输入上下文，枚举时串行复用 */
static struct trb cmd_ring[NCMD_TRB] __attribute__((aligned(64)));
static struct trb evt_ring[NEVT_TRB] __attribute__((aligned(64)));
static u64 erst[2]         __attribute__((aligned(64)));
static struct trb ep0_ring[MAX_HID][NEP0_TRB] __attribute__((aligned(64)));
static struct trb hid_ring[MAX_HID][NHID_TRB] __attribute__((aligned(64)));
static u8  ctl_buf[512]    __attribute__((aligned(64)));
static u8  hid_buf[MAX_HID][8] __attribute__((aligned(64)));
/* ---------- 运行时状态 ---------- */
static volatile u32 *xhci_cap;      /* capability 寄存器基址（BAR0） */
static volatile u32 *xhci_op;       /* operational 寄存器基址 */
static volatile u32 *xhci_rt;       /* runtime 寄存器基址 */
static volatile u32 *xhci_db;       /* doorbell 基址 */
static u32 cap_len;

static int  cmd_enq;                /* 命令环入队索引 */
static u32  cmd_cycle = 1;          /* 命令环当前 cycle */
static int  evt_deq;                /* 事件环出队索引 */
static u32  evt_cycle = 1;          /* 事件环期望 cycle */

static struct hid_dev G_dev[MAX_HID];
static int  G_cur;                  /* 当前正在做控制传输 / 端点操作的设备下标 */
static int  G_kbd_cnt;              /* 枚举到的键盘数 */
static int  G_ptr_cnt;              /* 枚举到的指针设备数（相对鼠标 + 绝对平板） */
/* ---------- 小工具 ---------- */
static inline void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* ⚠️⚠️ MMIO 访问必须走这两个函数，绝不要写 `*p` / `*p = v` 的直接解引用 ⚠️⚠️
   踩过的坑（2026-10-24 第十四轮真机复现）：
   GCC 在循环里会把「读寄存器 + 指针前进」强度削减成**后索引寻址** `ldr w3, [x1], #16`。
   带写回的加载/存储，硬件在 stage-2 数据中止里 **ISV=0**（指令综合信息不可用）→ KVM 走 NISV 路径
   → QEMU 解不出这笔访问 → 直接给 guest 注入一个 data abort（ESR=0x96000010，看着像"对齐错误"）。
   表现：TCG 一切正常（不经过 KVM 那套），KVM/真机读 PORTSC 必炸、且只有循环里的访问炸
   （单次访问照常编译成普通 ldr，所以 BAR 前段寄存器读得好好的）。
   宿主机取证：`kvm_guest_fault ... hsr 0x92000005`（ISV=0）vs 正常的 UART `hsr 0x93810006`（ISV=1）。 */
static u32 rd32(volatile u32 *p)
{
    u32 v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"((u64)p) : "memory");
    return v;
}
static void wr32(volatile u32 *p, u32 v)
{
    __asm__ volatile("str %w1, [%0]" :: "r"((u64)p), "r"(v) : "memory");
    barrier();
}

static void put(const char *s) { kputs(s); }
static void puthex(u64 v) { kputhex(v); }
static void putdec(u32 v) { kputdec(v); }

/* CPU 计数器（超时用；不依赖 timer_init 是否已跑） */
static inline u64 cntpct(void) { u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline u64 cntfrq(void) { u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

/* 输入上下文清零：in_ctx 是 64 字节对齐的 4KB 数组，8 字节一次写（原逐字节 = 4096 次）
   ⚠️ 每次枚举一个端口要清 1~3 次，多设备机器上会跑十几遍，别小看 */
static void in_ctx_zero(void)
{
    u64 *p = (u64 *)in_ctx;
    for (int i = 0; i < (int)(sizeof in_ctx / 8); i++) p[i] = 0;
}

/* ---------- MMU：把 PCI 窗口映射成 Device ---------- */
/* kernel.c 的 l2_dev 是 static 的，这里只放声明 + 用它提供的注册函数。
   实际映射在 kernel.c 里做（见 mmu_init 的 PCI 段），驱动只负责使用。 */

/* ---------- PCI ECAM 枚举 ---------- */
static inline volatile u32 *ecam_ptr(int bus, int dev, int fn, int reg)
{
    return (volatile u32 *)(PCI_ECAM_BASE
                            + ((u64)bus << 20) + ((u64)dev << 15)
                            + ((u64)fn << 12) + (reg & 0xffc));
}

/* ⚠️ 必须走 rd32/wr32 的内联汇编，不能写 *p：
   循环里的直接解引用会被 GCC 强度削减成带写回的 ldr/str（后索引寻址），
   KVM stage-2 下该访问 ISV=0 → NISV data abort（见本文件头踩坑记录）。
   2026-11-07 飞牛虚拟机刷数据中止，这两处是唯一还没改到的 ECAM 访问。 */
static int pci_cfg_read32(int bus, int dev, int fn, int reg)
{
    return (int)rd32(ecam_ptr(bus, dev, fn, reg));
}

static void pci_cfg_write32(int bus, int dev, int fn, int reg, u32 val)
{
    wr32(ecam_ptr(bus, dev, fn, reg), val);   /* wr32 内含 barrier() */
}

/* 找一个 xHCI 控制器；返回 bus/dev/fn，失败返回 -1
 *
 * ⚠️ 必须从根总线 0 出发、沿 PCI 桥（含 PCIe root port）递归扫：
 *    直接盲扫 bus 0-15 会访问"不存在"的总线 → QEMU 直接给外部中止
 *    （实测 FAR=0x3f41a000, ESR EC=0x25 DFSC=0x10，内核数据中止）。
 *    设备挂在 root port 后面时（飞牛虚拟机的 xhci 就在 bus 3），
 *    桥的 secondary bus 号在配置空间 0x19。 */
static int pci_find_xhci(int *bus_out, int *dev_out, int *fn_out)
{
    int buses[16];
    int nbus = 1;
    buses[0] = 0;

    for (int bi = 0; bi < nbus && nbus < 16; bi++) {
        int bus = buses[bi];
        for (int dev = 0; dev < 32; dev++) {
            int multi = 0;
            for (int fn = 0; fn < (multi ? 8 : 1); fn++) {
                int vid = pci_cfg_read32(bus, dev, fn, PCI_VENDOR_ID);
                if (vid == 0xffff || vid == 0)
                    continue;
                int cls = pci_cfg_read32(bus, dev, fn, PCI_CLASS);
                u32 class = ((u32)cls >> 16) & 0xffff;
                int hdr = pci_cfg_read32(bus, dev, fn, PCI_HEADER_TYPE);

                if (fn == 0 && (((u32)hdr >> 16) & 0x80))
                    multi = 1;

                /* USB 控制器 + prog-if 0x30 = xHCI */
                if (class == 0x0C03 && (((u32)cls >> 8) & 0xff) == 0x30) {
                    *bus_out = bus; *dev_out = dev; *fn_out = fn;
                    return 0;
                }

                /* PCI-to-PCI 桥（0x0604）：把 secondary bus 加进待扫队列。
                   总线号在配置空间 0x18 的 bits 15:8（0x19 字节） */
                if (class == 0x0604) {
                    int binfo = pci_cfg_read32(bus, dev, fn, 0x18);
                    int sec = ((u32)binfo >> 8) & 0xff;
                    if (sec && nbus < 16) {
                        int dup = 0;
                        for (int k = 0; k < nbus; k++)
                            if (buses[k] == sec) { dup = 1; break; }
                        if (!dup)
                            buses[nbus++] = sec;
                    }
                }
            }
        }
    }
    return -1;
}

/* BAR0 探测 + 必要时自己分配。
 * UEFI(ISO) 启动时固件已分配好 BAR，直接读用；
 * -kernel 直载没有固件，BAR 全是 0 → 这里探测大小、在 PCI MMIO 窗口里分配一个地址并写回去。 */
static u64 pci_setup_bar0(int bus, int dev, int fn)
{
    u32 lo = (u32)pci_cfg_read32(bus, dev, fn, PCI_BAR0);
    u64 bar;
    int is64 = ((lo & 0x6) == 0x4);

    if (is64) {
        u32 hi = (u32)pci_cfg_read32(bus, dev, fn, PCI_BAR0 + 4);
        bar = ((u64)hi << 32) | (lo & ~0xFUL);
    } else {
        bar = lo & ~0xFUL;
    }

    if (bar)
        return bar;                       /* 固件分配过了 */

    /* 探测大小 */
    pci_cfg_write32(bus, dev, fn, PCI_BAR0, 0xFFFFFFFF);
    if (is64) pci_cfg_write32(bus, dev, fn, PCI_BAR0 + 4, 0xFFFFFFFF);
    u32 mask_lo = (u32)pci_cfg_read32(bus, dev, fn, PCI_BAR0);
    u32 mask_hi = is64 ? (u32)pci_cfg_read32(bus, dev, fn, PCI_BAR0 + 4) : 0;
    if (is64) {
        u64 mask = ((u64)mask_hi << 32) | (mask_lo & ~0xFUL);
        u32 sz = (u32)((~mask) + 1);
        if (!sz) sz = 0x4000;
        if (sz & (sz - 1)) {              /* 对齐到 2 的幂 */
            u32 p = 1;
            while (p < sz) p <<= 1;
            sz = p;
        }
        bar = PCI_MMIO_BASE + 0x100000;   /* 固定落在 MMIO 窗口里，不跟固件打架（直载时没有别的设备） */
        pci_cfg_write32(bus, dev, fn, PCI_BAR0, (u32)bar);
        pci_cfg_write32(bus, dev, fn, PCI_BAR0 + 4, (u32)(bar >> 32));
    } else {
        u32 mask = mask_lo & ~0xFUL;
        u32 sz = (~mask) + 1;
        if (!sz) sz = 0x4000;
        bar = PCI_MMIO_BASE + 0x100000;
        pci_cfg_write32(bus, dev, fn, PCI_BAR0, (u32)bar);
    }
    return bar;
}

static void cmd_doorbell(void)
{
    barrier();
    xhci_db[0] = 0;   /* doorbell 0 = 命令环 */
    barrier();
}

static int cmd_submit(u64 param, u32 status, u32 control)
{
    struct trb *t = &cmd_ring[cmd_enq];
    t->param = param;
    t->status = status;
    t->control = (control & ~TRB_C) | (cmd_cycle ? TRB_C : 0);
    cmd_enq++;
    if (cmd_enq == NCMD_TRB - 1) {   /* 最后一个位置留给 Link TRB */
        struct trb *l = &cmd_ring[NCMD_TRB - 1];
        l->param = (u64)&cmd_ring[0];
        l->status = 0;
        l->control = (TR_LINK << TRB_TYPE_SH) |
                     /* TC: 翻转 cycle */ (1u << 1) |
                     (cmd_cycle ? TRB_C : 0);
        cmd_enq = 0;
        cmd_cycle ^= 1;
    }
    cmd_doorbell();
    return 0;
}

/* ---------- 事件环 ---------- */
static int evt_poll_one(u32 *type_out, u32 *ccode_out, u32 *slot_out, u32 *ep_out, u64 *param_out)
{
    struct trb *t = &evt_ring[evt_deq];
    u32 ctl = t->control;
    if ((ctl & TRB_C) != (evt_cycle ? TRB_C : 0))
        return 0;                       /* 没有新事件 */

    barrier();
    if (type_out)   *type_out = (ctl >> TRB_TYPE_SH) & TRB_TYPE_M;
    if (ccode_out)  *ccode_out = (t->status >> 24) & 0xff;
    if (slot_out)   *slot_out = (ctl >> TRB_SLOTID_SH) & 0xff;
    if (ep_out)     *ep_out = (ctl >> TRB_EPID_SH) & 0x1f;
    if (param_out)  *param_out = t->param;

    evt_deq++;
    if (evt_deq == NEVT_TRB) {
        evt_deq = 0;
        evt_cycle ^= 1;
    }
    /* 把 ERDP 推到下一个待读位置（QEMU 靠它判环满）
       ⚠️ 两次独立 wr32：`p[0]=…; p[1]=…` 会被 GCC 合成 stp/带写回的 str（ISV=0，KVM 下炸） */
    u64 erdp = (u64)&evt_ring[evt_deq];
    wr32((volatile u32 *)((u8 *)xhci_rt + XRT_ERDP(0)), (u32)erdp);
    wr32((volatile u32 *)((u8 *)xhci_rt + XRT_ERDP(0) + 4), (u32)(erdp >> 32));
    barrier();
    return 1;
}

/* 等一个指定类型的完成事件（命令用） */
static int evt_wait_cmd(u32 *ccode_out, u32 *slot_out)
{
    u64 deadline = cntpct() + cntfrq() * 5;   /* 5 秒超时 */
    while (cntpct() < deadline) {
        u32 type, cc, slot;
        if (evt_poll_one(&type, &cc, &slot, 0, 0)) {
            if (type == ER_CMD_COMPLETE) {
                if (ccode_out) *ccode_out = cc;
                if (slot_out)  *slot_out = slot;
                return 1;
            }
            /* 其它事件（端口状态变化等）顺手忽略 */
        }
    }
    return 0;
}

/* 等传输事件（EP0 / 键盘） */
static int evt_wait_transfer(int epid, u32 *ccode_out, u32 *len_out)
{
    u64 deadline = cntpct() + cntfrq() * 5;
    while (cntpct() < deadline) {
        u32 type, cc, slot, ep;
        u64 param;
        if (evt_poll_one(&type, &cc, &slot, &ep, &param)) {
            if (type == ER_TRANSFER && (int)ep == epid) {
                if (ccode_out) *ccode_out = cc;
                if (len_out)   *len_out = param & 0xffffff;   /* 事件长度不一定可靠 */
                return 1;
            }
            if (type == ER_PORT_STATUS) {
                /* 忽略，稍后统一处理端口 */
            }
        }
    }
    return 0;
}
/* ---------- 控制传输（EP0） ---------- */
/* ⚠️ 多设备：EP0 环按设备下标分开，doorbell 也要打在对应 slot 上。
   ctrl_xfer 全程由 G_cur 指定「现在操作哪个设备」，枚举是串行的，不会并发。 */
static int ep0_submit_trb(u32 type, u64 param, u32 status, u32 control_extra)
{
    struct hid_dev *d = &G_dev[G_cur];
    struct trb *t = &ep0_ring[G_cur][d->ep0_enq];
    t->param = param;
    t->status = status;
    t->control = (type << TRB_TYPE_SH) | control_extra |
                 (d->ep0_cycle ? TRB_C : 0);
    d->ep0_enq++;
    if (d->ep0_enq == NEP0_TRB - 1) {
        struct trb *l = &ep0_ring[G_cur][NEP0_TRB - 1];
        l->param = (u64)&ep0_ring[G_cur][0];
        l->status = 0;
        l->control = (TR_LINK << TRB_TYPE_SH) | (1u << 1) |
                     (d->ep0_cycle ? TRB_C : 0);
        d->ep0_enq = 0;
        d->ep0_cycle ^= 1;
    }
    return 0;
}


static void ep_doorbell(int dci)
{
    barrier();
    xhci_db[G_dev[G_cur].slot] = (u32)dci;   /* doorbell[slot] = 端点 DCI */
    barrier();
}

/* 控制传输：setup + 可选 data + status；返回 0 成功 */
static int ctrl_xfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex,
                     u16 wLength, void *data)
{
    int in = (bmRequestType & 0x80) != 0;
    u64 setup = (u64)bmRequestType
              | ((u64)bRequest << 8)
              | ((u64)wValue << 16)
              | ((u64)wIndex << 32)
              | ((u64)wLength << 48);

    /* Setup Stage：IDT，TRT=2(IN)/3(OUT)，长度 8 */
    u32 trt = wLength ? (in ? 2u : 3u) : 0u;
    ep0_submit_trb(TR_SETUP, setup, 8,
                   TRB_IDT | (trt << 16) | (wLength ? TRB_CH : 0));

    /* Data Stage */
    if (wLength) {
        ep0_submit_trb(TR_DATA, (u64)data, wLength,
                       (in ? TRB_DIR_IN : 0) | TRB_CH);
    }

    /* Status Stage：方向与数据相反；IOC 在这里等事件 */
    u32 sdir = (wLength && in) ? 0 : TRB_DIR_IN;
    ep0_submit_trb(TR_STATUS, 0, 0, sdir | TRB_IOC);

    ep_doorbell(1);   /* EP0 = DCI 1 */

    u32 cc = 0;
    if (!evt_wait_transfer(1, &cc, 0)) {
        put("[xhci] ctrl 超时\n");
        return -1;
    }
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) {
        put("[xhci] ctrl 失败 cc=");
        puthex(cc);
        put("\n");
        return -1;
    }
    return 0;
}
/* ---------- 设备枚举 ---------- */
struct usb_dev_desc {
    u8  bLength; u8 bDescriptorType; u16 bcdUSB;
    u8  bDeviceClass; u8 bDeviceSubClass; u8 bDeviceProtocol; u8 bMaxPacketSize0;
    u16 idVendor; u16 idProduct; u16 bcdDevice;
    u8  iManufacturer; u8 iProduct; u8 iSerialNumber; u8 bNumConfigurations;
} __attribute__((packed));

struct usb_conf_desc {
    u8  bLength; u8 bDescriptorType; u16 wTotalLength;
    u8  bNumInterfaces; u8 bConfigurationValue; u8 iConfiguration; u8 bmAttributes;
    u8  bMaxPower;
} __attribute__((packed));

struct usb_iface_desc {
    u8 bLength; u8 bDescriptorType;
    u8 bInterfaceNumber; u8 bAlternateSetting;
    u8 bNumEndpoints; u8 bInterfaceClass; u8 bInterfaceSubClass; u8 bInterfaceProtocol;
    u8 iInterface;
} __attribute__((packed));

struct usb_ep_desc {
    u8  bLength; u8 bDescriptorType;
    u8  bEndpointAddress; u8 bmAttributes;
    u16 wMaxPacketSize; u8 bInterval;
} __attribute__((packed));

#define USB_DT_DEVICE    1
#define USB_DT_CONFIG    2
#define USB_DT_IFACE     4
#define USB_DT_ENDPOINT  5

static int get_desc(u8 type, u8 index, u16 len, void *buf)
{
    return ctrl_xfer(0x80, 6, (u16)((type << 8) | index), 0, len, buf);
}
/* 解析配置描述符，识别三类 HID 设备。
   判据逐字段核对 QEMU 8.2.2 hw/usb/dev-hid.c 的 desc_iface_* ：
     keyboard : class=3 subclass=1 proto=1  wMaxPacketSize=8 → 8B boot 报告
     mouse    : class=3 subclass=1 proto=2  wMaxPacketSize=4 → 4B 相对报告
                (buttons, dx, dy, wheel)
     tablet   : class=3 subclass=0 proto=0  wMaxPacketSize=8 → 6B 绝对报告
                (buttons, x_lo, x_hi, y_lo, y_hi, wheel)，坐标 0..32767
   真机 VM 三个设备全在同一个 qemu-xhci 上，端口顺序 tablet/mouse/keyboard，
   所以必须逐个端口枚举、按类型分派，不能「找到第一个键盘就收工」。 */
static int parse_config(u8 *buf, int total, int *ep_num, u16 *maxpkt,
                        u8 *interval, int *kind)
{
    int off = 0;
    int cur_iface = -1, hid_iface = -1, want = 0;

    while (off + 2 <= total) {
        u8 len = buf[off];
        u8 type = buf[off + 1];
        if (!len)
            break;
        if (type == USB_DT_IFACE && len >= 9) {
            struct usb_iface_desc *d = (struct usb_iface_desc *)(buf + off);
            cur_iface = d->bInterfaceNumber;
            hid_iface = -1; want = 0;
            if (d->bInterfaceClass == 3) {
                if (d->bInterfaceSubClass == 1 && d->bInterfaceProtocol == 1) {
                    hid_iface = cur_iface; want = 1;      /* 键盘 */
                } else if (d->bInterfaceSubClass == 1 && d->bInterfaceProtocol == 2) {
                    hid_iface = cur_iface; want = 2;      /* 相对鼠标 */
                } else if (d->bInterfaceProtocol == 0) {
                    hid_iface = cur_iface; want = 3;      /* 绝对平板 */
                }
            }
        } else if (type == USB_DT_ENDPOINT && len >= 7 &&
                   cur_iface == hid_iface && hid_iface >= 0) {
            struct usb_ep_desc *e = (struct usb_ep_desc *)(buf + off);
            u16 mps = e->wMaxPacketSize & 0x7ff;
            if ((e->bmAttributes & 3) == 3 && (e->bEndpointAddress & 0x80)) {
                /* 平板报告 6B，端点至少要有 6B；键盘/鼠标天然满足 */
                if (!(want == 3 && mps < 6)) {
                    *ep_num = e->bEndpointAddress & 0x0f;
                    *maxpkt = mps;
                    *interval = e->bInterval;
                    *kind = want;
                    return 0;
                }
            }
        }
        off += len;
    }
    return -1;
}

/* ---------- 输入上下文填充 ---------- */
static u32 *ictx_slot(void)  { return (u32 *)(in_ctx + 32); }
static u32 *ictx_ep(int dci) { return (u32 *)(in_ctx + 32 + 32 * dci); }

/* slot context: route string(0) | speed(20:23) | context entries(27:31) */
static void set_slot_ctx(int entries, int port, int speed)
{
    u32 *s = ictx_slot();
    for (int i = 0; i < 4; i++) s[i] = 0;
    s[0] = (u32)speed << 20;                    /* route string = 0 */
    s[0] |= (u32)entries << 27;                 /* context entries */
    s[1] = (u32)port << 16;                     /* root hub port number */
}

static void set_ep_ctx(int dci, int type, int maxpkt, int interval, u64 ring, int cycle)
{
    u32 *e = ictx_ep(dci);
    for (int i = 0; i < 8; i++) e[i] = 0;
    e[0] = ((u32)interval & 0xff) << 16;        /* interval */
    e[1] = ((u32)type & 7) << 3;                /* endpoint type */
    e[1] |= ((u32)maxpkt) << 16;                /* max packet size */
    e[2] = (u32)(ring & 0xfffffff0UL) | (cycle ? 1u : 0u);   /* dequeue low + DCS */
    e[3] = (u32)(ring >> 32);                   /* dequeue high */
    e[4] = 8;                                   /* avg TRB length */
}

/* ---------- 端口复位 ---------- */
static int port_reset(int port, int *speed_out)
{
    u64 deadline;

    /* 等 CCS 出现 */
    deadline = cntpct() + cntfrq() * 3;
    while (cntpct() < deadline) {
        u32 ps = xhci_op[XOP_PORTSC(port) / 4];
        if (ps & PORTSC_CCS)
            break;
    }
    u32 ps = xhci_op[XOP_PORTSC(port) / 4];
    if (!(ps & PORTSC_CCS))
        return -1;

    /* PR=1 复位 */
    xhci_op[XOP_PORTSC(port) / 4] = ps | PORTSC_PR;
    barrier();

    /* 等 PRC */
    deadline = cntpct() + cntfrq() * 3;
    while (cntpct() < deadline) {
        ps = xhci_op[XOP_PORTSC(port) / 4];
        if (ps & PORTSC_PRC)
            break;
    }
    if (!(ps & PORTSC_PRC))
        return -1;

    /* 清 PRC/CSC/PEC（写 1 清） */
    xhci_op[XOP_PORTSC(port) / 4] = ps | PORTSC_PRC | PORTSC_CSC | PORTSC_PEC;
    barrier();

    ps = xhci_op[XOP_PORTSC(port) / 4];
    *speed_out = (ps >> PORTSC_SPEED_SH) & PORTSC_SPEED_M;
    if (!(ps & PORTSC_PED))
        return -1;
    return 0;
}
/* ---------- 键盘报告 → ASCII ---------- */
/* HID usage ID (USB HID Usage Tables 10, Keyboard/Keypad) → ASCII */
static const char hid_map[128] = {
    /* 0x00-0x03 */ 0, 0, 0, 0,
    /* 0x04-0x1d: a-z */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
                        'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1e-0x27: 1-9,0 */ '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 Enter */ '\n',
    /* 0x29 Esc */ 27,
    /* 0x2a Backspace */ '\b',
    /* 0x2b Tab */ '\t',
    /* 0x2c Space */ ' ',
    /* 0x2d -_ */ '-',
    /* 0x2e =+ */ '=',
    /* 0x2f [{ */ '[',
    /* 0x30 ]} */ ']',
    /* 0x31 \| */ '\\',
    /* 0x32 (no shift) */ 0,
    /* 0x33 ;: */ ';',
    /* 0x34 '" */ '\'',
    /* 0x35 `~ */ '`',
    /* 0x36 ,< */ ',',
    /* 0x37 .> */ '.',
    /* 0x38 /? */ '/',
    /* 0x4c Delete：吞掉太反直觉，当退格用（shell 把 127 也当退格）。
       ⚠️ 真正的「前向删除」需要行编辑器支持插入点，暂不做。 */
    [0x4c] = 127,
};
/* shift 映射（仅数字和标点） */
static const char hid_shift_map[128] = {
    [0x1e]='!',[0x1f]='@',[0x20]='#',[0x21]='$',[0x22]='%',
    [0x23]='^',[0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',
    [0x2d]='_',[0x2e]='+',[0x2f]='{',[0x30]='}',[0x31]='|',
    [0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',[0x38]='?',
};

static char hid_to_ascii(int usage, int shift)
{
    if (usage >= 0x04 && usage <= 0x1d) {          /* a-z */
        char c = (char)('a' + usage - 0x04);
        return shift ? (char)(c - 'a' + 'A') : c;
    }
    if (usage < 128) {
        if (shift && hid_shift_map[usage])
            return hid_shift_map[usage];
        return hid_map[usage];
    }
    return 0;
}
/* ---------- HID 报告解析 ---------- */
/* 键盘：Ctrl+Shift+F1 组合键。
   HID modifier 位：bit0 LCtrl bit1 LShift bit4 RCtrl bit5 RShift；F1 usage = 0x3A。
   组合键没有 ASCII 字符，直接置「请求退出桌面」标志，由 desktop_run 消费。 */
static void kbd_report(struct hid_dev *d, const u8 *rep)
{
    u8 mod = rep[0];
    int shift = (mod & 0x22) != 0;
    int ctrl  = (mod & 0x11) != 0;
    int cur[6];
    for (int i = 0; i < 6; i++)
        cur[i] = rep[2 + i];

    /* 新按下 = 在当前报告里但不在上一份里 */
    for (int i = 0; i < 6; i++) {
        int k = cur[i];
        if (!k || k == 1)             /* 1 = rollover error */
            continue;
        int seen = 0;
        for (int j = 0; j < 6; j++)
            if (d->prev_keys[j] == k) { seen = 1; break; }
        if (seen) continue;
        if (k == 0x3A && ctrl && shift) {
            desktop_hotkey_exit();
            continue;
        }
        char c = hid_to_ascii(k, shift);
        if (c)
            kbd_feed(c);
    }
    for (int i = 0; i < 6; i++)
        d->prev_keys[i] = (u8)cur[i];
    d->prev_mod = mod;
}

/* 指针设备：QEMU hw/input/hid.c 的 hid_pointer_poll() 报告格式
   HID_MOUSE   (相对) 4B: buttons, dx(int8), dy(int8), wheel(int8)
   HID_TABLET  (绝对) 6B: buttons, x_lo, x_hi, y_lo, y_hi, wheel
   buttons bit0=左 bit1=右 bit2=中。绝对坐标范围 0..32767（QEMU_INPUT_ABS_MAX）。 */
static void pointer_report(struct hid_dev *d, const u8 *rep)
{
    int btn = rep[0] & 7;
    if (d->kind == 2) {
        int dx = (int)(signed char)rep[1];
        int dy = (int)(signed char)rep[2];
        int wh = (int)(signed char)rep[3];
        mouse_feed_rel(dx, dy, btn, wh);
    } else {
        int ax = (int)rep[1] | ((int)rep[2] << 8);
        int ay = (int)rep[3] | ((int)rep[4] << 8);
        int wh = (int)(signed char)rep[5];
        mouse_feed_abs(ax, ay, btn, wh);
    }
    d->prev_btn = (u8)btn;
}

/* ---------- 中断端点：投递 / 收割 ---------- */
static void hid_arm(int idx)
{
    struct hid_dev *d = &G_dev[idx];
    struct trb *t = &hid_ring[idx][d->enq];
    t->param = (u64)hid_buf[idx];
    t->status = d->maxpkt;           /* 请求设备声明的最大包长 */
    t->control = (TR_NORMAL << TRB_TYPE_SH) | TRB_IOC |
                 (d->cycle ? TRB_C : 0);
    d->enq++;
    if (d->enq == NHID_TRB - 1) {
        struct trb *l = &hid_ring[idx][NHID_TRB - 1];
        l->param = (u64)&hid_ring[idx][0];
        l->status = 0;
        l->control = (TR_LINK << TRB_TYPE_SH) | (1u << 1) |
                     (d->cycle ? TRB_C : 0);
        d->enq = 0;
        d->cycle ^= 1;
    }
    G_cur = idx;
    ep_doorbell(d->dci);
}

/* ---------- 公开接口 ---------- */
/* 一个事件环服务所有设备：按 (slot, dci) 反查设备，处理报告后重新投递。
   没有指针事件时 QEMU 的 HID 设备回 NAK，xHCI 不会产生事件，不占 CPU。 */
void usb_kbd_poll(void)
{
    for (;;) {
        u32 type, cc, slot, ep;
        u64 param;
        if (!evt_poll_one(&type, &cc, &slot, &ep, &param))
            break;
        if (type != ER_TRANSFER)
            continue;
        int idx = -1;
        for (int i = 0; i < MAX_HID; i++)
            if (G_dev[i].ready && G_dev[i].slot == (int)slot &&
                G_dev[i].dci == (int)ep) { idx = i; break; }
        if (idx < 0)
            continue;
        struct hid_dev *d = &G_dev[idx];
        if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
            if (d->kind == 1)      kbd_report(d, hid_buf[idx]);
            else if (d->kind >= 2) pointer_report(d, hid_buf[idx]);
        }
        hid_arm(idx);               /* 重新投递下一个请求 */
    }
}

int usb_kbd_init(void)
{
    int bus, dev, fn;

    /* 1. 找 xHCI */
    if (pci_find_xhci(&bus, &dev, &fn) < 0) {
        put("[usb ] 没找到 xHCI 控制器（虚拟机没插 USB 控制器？）\n");
        return -1;
    }
    put("[usb ] 找到 xHCI: bus="); putdec(bus);
    put(" dev="); putdec(dev); put(" fn="); putdec(fn); put("\n");

    /* 2. BAR0（没有固件时自己分配）+ 使能 Memory/BusMaster */
    u64 bar = pci_setup_bar0(bus, dev, fn);
    if (!bar) {
        put("[usb ] BAR0 分配失败\n");
        return -1;
    }
    pci_cfg_write32(bus, dev, fn, PCI_COMMAND, PCI_CMD_MSE | PCI_CMD_BME);
    put("[usb ] BAR0="); puthex(bar); put("\n");

    /* 3. 寄存器基址 */
    xhci_cap = (volatile u32 *)bar;
    u8 caplen = (u8)(xhci_cap[XCAP_CAPLENGTH / 4] & 0xff);
    cap_len = caplen;
    xhci_op = (volatile u32 *)((u8 *)xhci_cap + caplen);
    u32 rtsoff = (rd32((volatile u32 *)((u8 *)xhci_cap + XCAP_RTSOFF)) >> 0) & 0xffffffe0UL;
    u32 dboff  = (rd32((volatile u32 *)((u8 *)xhci_cap + XCAP_DBOFF)) >> 0) & 0xfffffffcUL;
    xhci_rt = (volatile u32 *)((u8 *)xhci_cap + rtsoff);
    xhci_db = (volatile u32 *)((u8 *)xhci_cap + dboff);
    put("[usb ] CAPLENGTH="); putdec(caplen);
    put(" RTSOFF="); puthex(rtsoff);
    put(" DBOFF="); puthex(dboff); put("\n");

    u32 hcs1 = rd32((volatile u32 *)((u8 *)xhci_cap + XCAP_HCSPARAMS1));
    u32 hcc1 = rd32((volatile u32 *)((u8 *)xhci_cap + XCAP_HCCPARAMS1));
    int maxslots = hcs1 & 0xff;
    int maxports = (hcs1 >> 24) & 0xff;
    put("[usb ] HCSPARAMS1="); puthex(hcs1);
    put(" (slots="); putdec(maxslots);
    put(" ports="); putdec(maxports); put(")");
    put(" HCCPARAMS1="); puthex(hcc1); put("\n");

    /* 4. 复位控制器 */
    wr32(xhci_op + XOP_USBCMD / 4, 0);            /* 先停 */
    u64 deadline = cntpct() + cntfrq() * 2;
    while (!(rd32(xhci_op + XOP_USBSTS / 4) & USBSTS_HCH)) {
        if (cntpct() > deadline) { put("[usb ] 停机超时\n"); return -1; }
    }
    wr32(xhci_op + XOP_USBCMD / 4, USBCMD_HCRST);
    deadline = cntpct() + cntfrq() * 2;
    while (rd32(xhci_op + XOP_USBCMD / 4) & USBCMD_HCRST) {
        if (cntpct() > deadline) { put("[usb ] 复位超时\n"); return -1; }
    }
    put("[usb ] 控制器已复位\n");

    /* 5. DCBAA + CONFIG */
    for (int i = 0; i <= 64; i++)
        dcbaa[i] = 0;
    dcbaa[0] = 0;                                  /* 无 scratchpad */
    wr32(xhci_op + XOP_DCBAAP / 4, (u32)(u64)dcbaa);
    wr32(xhci_op + XOP_DCBAAP / 4 + 1, (u32)((u64)dcbaa >> 32));
    wr32(xhci_op + XOP_CONFIG / 4, (u32)(maxslots & 0xff));

    /* 6. 命令环 */
    for (int i = 0; i < NCMD_TRB; i++) {
        cmd_ring[i].param = 0; cmd_ring[i].status = 0; cmd_ring[i].control = 0;
    }
    cmd_enq = 0;
    cmd_cycle = 1;
    for (int i = 0; i < NCMD_TRB - 1; i++)
        cmd_ring[i].control = 0;                   /* cycle=0：等我们写 */
    cmd_ring[NCMD_TRB - 1].param = (u64)&cmd_ring[0];
    cmd_ring[NCMD_TRB - 1].control = (TR_LINK << TRB_TYPE_SH) | (1u << 1);
    wr32(xhci_op + XOP_CRCR / 4, (u32)(u64)cmd_ring | 1);   /* RCS=1 */
    wr32(xhci_op + XOP_CRCR / 4 + 1, (u32)((u64)cmd_ring >> 32));

    /* 7. 事件环 */
    for (int i = 0; i < NEVT_TRB; i++) {
        evt_ring[i].param = 0; evt_ring[i].status = 0; evt_ring[i].control = 0;
    }
    evt_deq = 0;
    evt_cycle = 1;
    erst[0] = (u64)evt_ring;
    erst[1] = (u64)NEVT_TRB | ((u64)NEVT_TRB << 32);
    wr32((volatile u32 *)((u8 *)xhci_rt + XRT_ERSTSZ(0)), 1);
    wr32((volatile u32 *)((u8 *)xhci_rt + XRT_ERSTBA(0)), (u32)(u64)erst);
    wr32((volatile u32 *)((u8 *)xhci_rt + XRT_ERSTBA(0) + 4), (u32)((u64)erst >> 32));
    wr32((volatile u32 *)((u8 *)xhci_rt + XRT_ERDP(0)), (u32)(u64)evt_ring);
    wr32((volatile u32 *)((u8 *)xhci_rt + XRT_ERDP(0) + 4), (u32)((u64)evt_ring >> 32));
    barrier();
    /* 8. 开跑 */
    wr32(xhci_op + XOP_USBCMD / 4, USBCMD_RS);
    deadline = cntpct() + cntfrq() * 2;
    while (rd32(xhci_op + XOP_USBSTS / 4) & USBSTS_HCH) {
        if (cntpct() > deadline) { put("[usb ] 启动超时\n"); return -1; }
    }
    put("[usb ] 控制器已运行\n");

    /* 9. 逐个端口枚举所有有设备的端口。
       每个设备拿自己的 slot（QEMU 返回 1,2,3...），用 slot-1 作为静态数组下标，
       这样即使中间某个设备枚举失败也不会复用别的设备的 DMA 区。 */
    struct usb_dev_desc dd;
    u8 cfg_head[9];
    struct usb_conf_desc *cd = (struct usb_conf_desc *)cfg_head;

    for (int p = 1; p <= maxports; p++) {
        u32 ps = rd32((volatile u32 *)((u8 *)xhci_op + XOP_PORTSC(p)));
        if (!(ps & PORTSC_CCS))
            continue;
        int port = p, speed = 0;
        put("[usb ] 端口 "); putdec(port); put(" 有设备，复位中...\n");
        if (port_reset(port, &speed) < 0) {
            put("[usb ] 端口复位失败，跳过\n");
            continue;
        }

        /* 10. Enable Slot */
        cmd_submit(0, 0, CR_ENABLE_SLOT << TRB_TYPE_SH);
        u32 cc = 0, sl = 0;
        if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
            put("[usb ] Enable Slot 失败 cc="); puthex(cc); put("\n");
            continue;
        }
        int slot = (int)sl;
        if (slot < 1 || slot > MAX_HID) {
            put("[usb ] slot 超出静态设备表，跳过: "); putdec((u32)slot); put("\n");
            continue;
        }
        int idx = slot - 1;
        G_cur = idx;
        struct hid_dev *d = &G_dev[idx];
        d->slot = slot; d->port = port; d->speed = speed;
        d->ready = 0; d->kind = 0;
        d->enq = 0; d->cycle = 1;
        d->ep0_enq = 0; d->ep0_cycle = 1;
        for (int i = 0; i < 6; i++) { d->prev_keys[i] = 0; }
        d->prev_mod = 0; d->prev_btn = 0;
        dcbaa[slot] = (u64)dev_ctx[idx];
        put("[usb ] slot="); putdec((u32)slot); put("\n");

        /* 11. Address Device */
        in_ctx_zero();
        set_slot_ctx(1, port, speed);
        for (int i = 0; i < NEP0_TRB; i++) {
            ep0_ring[idx][i].param = 0; ep0_ring[idx][i].status = 0;
            ep0_ring[idx][i].control = 0;
        }
        ep0_ring[idx][NEP0_TRB - 1].param = (u64)&ep0_ring[idx][0];
        ep0_ring[idx][NEP0_TRB - 1].control = (TR_LINK << TRB_TYPE_SH) | (1u << 1);
        set_ep_ctx(1, EP_TYPE_CTRL, 8, 0, (u64)ep0_ring[idx], 1);
        ((volatile u32 *)in_ctx)[1] = CTX_ADD_SLOT | CTX_ADD_EP0;
        cmd_submit((u64)in_ctx, 0, CR_ADDRESS_DEV << TRB_TYPE_SH |
                    ((u32)slot << TRB_SLOTID_SH));
        if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
            put("[usb ] Address Device 失败 cc="); puthex(cc); put("\n");
            continue;
        }
        put("[usb ] 设备已寻址\n");

        /* 12. 设备描述符（先 8B 拿 MPS0，必要时 Evaluate Context，再读完整 18B） */
        for (u32 i = 0; i < sizeof(dd); i++) ((u8 *)&dd)[i] = 0;
        if (get_desc(USB_DT_DEVICE, 0, 8, &dd) < 0) {
            put("[usb ] 取设备描述符(8B)失败\n");
            continue;
        }
        put("[usb ] MPS0="); putdec(dd.bMaxPacketSize0); put("\n");
        if (dd.bMaxPacketSize0 != 8) {
            in_ctx_zero();
            set_ep_ctx(1, EP_TYPE_CTRL, dd.bMaxPacketSize0, 0,
                       (u64)ep0_ring[idx], d->ep0_cycle);
            ((volatile u32 *)in_ctx)[0] = 0;
            ((volatile u32 *)in_ctx)[1] = CTX_ADD_EP0;
            cmd_submit((u64)in_ctx, 0, CR_EVAL_CONTEXT << TRB_TYPE_SH |
                        ((u32)slot << TRB_SLOTID_SH));
            if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
                put("[usb ] Evaluate Context 失败 cc="); puthex(cc); put("\n");
                continue;
            }
        }
        if (get_desc(USB_DT_DEVICE, 0, 18, &dd) < 0) {
            put("[usb ] 取设备描述符(18B)失败\n");
            continue;
        }
        put("[usb ] 设备 "); puthex(dd.idVendor); put(":"); puthex(dd.idProduct);
        put(" 类="); putdec(dd.bDeviceClass); put("\n");

        /* 13. 配置描述符 + 类型识别 */
        if (get_desc(USB_DT_CONFIG, 0, 9, cfg_head) < 0) {
            put("[usb ] 取配置头失败\n");
            continue;
        }
        cd = (struct usb_conf_desc *)cfg_head;
        int total = cd->wTotalLength;
        if (total < 9 || total > (int)sizeof(ctl_buf)) {
            put("[usb ] 配置长度异常\n");
            continue;
        }
        if (get_desc(USB_DT_CONFIG, 0, (u16)total, ctl_buf) < 0) {
            put("[usb ] 取配置失败\n");
            continue;
        }
        int kind = 0, ep_num = 0;
        u16 maxpkt = 0;
        u8 interval = 0;
        if (parse_config(ctl_buf, total, &ep_num, &maxpkt, &interval, &kind) < 0) {
            put("[usb ] 这个端口不是支持的 HID 设备，跳过\n");
            continue;
        }
        d->kind = (u8)kind; d->ep_num = ep_num;
        d->maxpkt = maxpkt; d->interval = interval;
        d->dci = (ep_num << 1) | 1;
        put("[usb ] 识别 kind="); putdec((u32)kind);
        put(" 端点 0x"); puthex((u64)ep_num);
        put(" maxpkt="); putdec(maxpkt); put("\n");

        /* 14. SET_CONFIGURATION */
        if (ctrl_xfer(0x00, 9, cd->bConfigurationValue, 0, 0, 0) < 0) {
            put("[usb ] SET_CONFIGURATION 失败\n");
            continue;
        }
        /* 15. SET_PROTOCOL(boot)：只对 boot 设备（键盘 / 相对鼠标）；平板是 report 协议 */
        if (kind != 3 && ctrl_xfer(0x21, 0x0B, 0, 0, 0, 0) < 0)
            put("[usb ] SET_PROTOCOL 失败（忽略）\n");

        /* 16. Configure Endpoint：加中断 IN 端点 */
        in_ctx_zero();
        set_slot_ctx(d->dci, d->port, d->speed);
        set_ep_ctx(1, EP_TYPE_CTRL, dd.bMaxPacketSize0 ? dd.bMaxPacketSize0 : 8,
                   0, (u64)ep0_ring[idx], d->ep0_cycle);
        for (int i = 0; i < NHID_TRB; i++) {
            hid_ring[idx][i].param = 0; hid_ring[idx][i].status = 0;
            hid_ring[idx][i].control = 0;
        }
        hid_ring[idx][NHID_TRB - 1].param = (u64)&hid_ring[idx][0];
        hid_ring[idx][NHID_TRB - 1].control = (TR_LINK << TRB_TYPE_SH) | (1u << 1);
        set_ep_ctx(d->dci, EP_TYPE_INT_IN, maxpkt, 0, (u64)hid_ring[idx], 1);
        ((volatile u32 *)in_ctx)[1] = CTX_ADD_SLOT | (1u << d->dci);
        cmd_submit((u64)in_ctx, 0, CR_CONFIG_EP << TRB_TYPE_SH |
                    ((u32)slot << TRB_SLOTID_SH));
        if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
            put("[usb ] Configure Endpoint 失败 cc="); puthex(cc); put("\n");
            continue;
        }

        /* 17. 标记就绪。⚠️ 这里**先不 arm** —— 枚举是串行的，
           evt_wait_cmd 会吞掉所有非命令完成事件；若提前 arm 了前面的设备，
           后面设备枚举期间到达的鼠标/键盘报告会被丢掉且不再投递（端点卡死）。
           所有设备配置完后再统一 arm，见循环之后。 */
        d->ready = 1;
        if (kind == 1) G_kbd_cnt++;
        if (kind >= 2) G_ptr_cnt++;
        put("[usb ] 设备就绪 slot="); putdec((u32)slot);
        put(" DCI="); putdec((u32)d->dci); put("\n");
    }

    /* 18. 统一投递所有中断请求：此刻事件环里不会再混进设备报告 */
    for (int i = 0; i < MAX_HID; i++)
        if (G_dev[i].ready) hid_arm(i);

    put("[usb ] HID 枚举完成：键盘="); putdec((u32)G_kbd_cnt);
    put(" 指针设备="); putdec((u32)G_ptr_cnt); put("\n");
    if (!G_kbd_cnt)
        put("[usb ] ⚠️ 没找到 USB 键盘，仍可用串口打字\n");
    if (!G_ptr_cnt)
        put("[usb ] ⚠️ 没找到鼠标/平板，桌面只能用键盘操作\n");
    return (G_kbd_cnt || G_ptr_cnt) ? 0 : -1;
}
