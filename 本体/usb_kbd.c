/* LoongOS 阶段6① —— xHCI 主机控制器 + USB HID boot 键盘驱动
 *
 * 目标：飞牛虚拟机（ramfb 屏幕 + qemu-xhci + usb-kbd）里能直接用键盘打字，
 *       字符塞进 kernel.c 的 ring 环形缓冲，shell 一行都不用改。
 *
 * 现实约束（写之前先看清楚）：
 *   - 内核无分配器：所有 DMA 结构用静态 64B 对齐数组（.bss，恒等映射 VA=PA）
 *   - 无 MSI-X：完全轮询事件环，不开 INE/IE；10Hz 定时器唤醒 wfi 时顺带 poll
 *   - 不依赖设备树：QEMU virt 的内存映射写死（ECAM 0x3f000000 等），与 GIC/UART 同款做法
 *   - DMA 一致性：QEMU TCG/KVM 的设备 DMA 与 guest CPU 在同一 host 一致性域，
 *     无需 cache 维护（真机 raspi3b 才需要 dc civac，届时再补）
 *
 * 参考：xHCI 1.1 规范 + Linux drivers/usb/host/xhci.h + QEMU hw/usb/hcd-xhci.c（逐字段核对过）
 */
#include "usb_kbd.h"

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---------- 内核侧接口（kernel.c 提供） ---------- */
extern void kputs(const char *s);      /* 串口 + 屏幕 */
extern void kputhex(u64 v);
extern void kputdec(u32 v);
extern void kbd_feed(char c);          /* 带临界区塞进 ring */

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
#define NKBD_TRB 8

static volatile u64 dcbaa[64 + 1] __attribute__((aligned(64)));
static u8  dev_ctx[2048]   __attribute__((aligned(64)));   /* 设备上下文（一个 slot） */
static u8  in_ctx[4096]    __attribute__((aligned(64)));   /* 输入上下文（含 EP 32 个槽位） */
static struct trb cmd_ring[NCMD_TRB] __attribute__((aligned(64)));
static struct trb evt_ring[NEVT_TRB] __attribute__((aligned(64)));
static u64 erst[2]         __attribute__((aligned(64)));   /* 16B: 段地址(8) + 保留(4) + 大小(4) */
static struct trb ep0_ring[NEP0_TRB] __attribute__((aligned(64)));
static struct trb kbd_ring[NKBD_TRB] __attribute__((aligned(64)));
static u8  ctl_buf[512]    __attribute__((aligned(64)));   /* 控制传输数据缓冲 */
static u8  kbd_buf[8]      __attribute__((aligned(64)));   /* HID 报告 8 字节 */

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

static int  ep0_enq;
static u32  ep0_cycle = 1;
static int  kbd_enq;
static u32  kbd_cycle = 1;

static int  slot_id;
static int  kbd_ep_num;             /* 键盘端点号（如 1） */
static int  kbd_ep_dci;             /* 端点 DCI（doorbell 用） */
static u16  kbd_maxpkt;
static u8   kbd_interval;
static int  kbd_prev_keys[6];       /* 上一份报告里的 6 个 keycode */
static u8   kbd_prev_mod;
static int  kbd_ready;

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

static int pci_cfg_read32(int bus, int dev, int fn, int reg)
{
    return (int)*ecam_ptr(bus, dev, fn, reg);
}

static void pci_cfg_write32(int bus, int dev, int fn, int reg, u32 val)
{
    *ecam_ptr(bus, dev, fn, reg) = val;
    barrier();
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

/* ---------- 命令环 ---------- */
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
static int ep0_submit_trb(u32 type, u64 param, u32 status, u32 control_extra)
{
    struct trb *t = &ep0_ring[ep0_enq];
    t->param = param;
    t->status = status;
    t->control = (type << TRB_TYPE_SH) | control_extra |
                 (ep0_cycle ? TRB_C : 0);
    ep0_enq++;
    if (ep0_enq == NEP0_TRB - 1) {
        struct trb *l = &ep0_ring[NEP0_TRB - 1];
        l->param = (u64)&ep0_ring[0];
        l->status = 0;
        l->control = (TR_LINK << TRB_TYPE_SH) | (1u << 1) |
                     (ep0_cycle ? TRB_C : 0);
        ep0_enq = 0;
        ep0_cycle ^= 1;
    }
    return 0;
}

static void ep_doorbell(int dci)
{
    barrier();
    xhci_db[slot_id] = (u32)dci;   /* doorbell[slot] = 端点 DCI */
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

/* 解析配置描述符，找 HID boot 键盘接口 */
static int parse_config(u8 *buf, int total, int *ep_num, u16 *maxpkt, u8 *interval)
{
    int off = 0;
    int cur_iface = -1;
    int kbd_iface = -1;

    while (off + 2 <= total) {
        u8 len = buf[off];
        u8 type = buf[off + 1];
        if (!len)
            break;
        if (type == USB_DT_IFACE && len >= 9) {
            struct usb_iface_desc *d = (struct usb_iface_desc *)(buf + off);
            cur_iface = d->bInterfaceNumber;
            if (d->bInterfaceClass == 3 &&      /* HID */
                d->bInterfaceSubClass == 1 &&   /* boot */
                d->bInterfaceProtocol == 1) {   /* keyboard */
                kbd_iface = d->bInterfaceNumber;
            }
        } else if (type == USB_DT_ENDPOINT && len >= 7 &&
                   cur_iface == kbd_iface) {
            struct usb_ep_desc *e = (struct usb_ep_desc *)(buf + off);
            if ((e->bmAttributes & 3) == 3 && (e->bEndpointAddress & 0x80)) {
                *ep_num = e->bEndpointAddress & 0x0f;
                *maxpkt = e->wMaxPacketSize & 0x7ff;
                *interval = e->bInterval;
                return 0;
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

/* 处理一份 8 字节 boot 报告 */
static void handle_report(const u8 *rep)
{
    u8 mod = rep[0];
    int shift = (mod & 0x22) != 0;   /* LShift | RShift */
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
            if (kbd_prev_keys[j] == k) { seen = 1; break; }
        if (!seen) {
            char c = hid_to_ascii(k, shift);
            if (c)
                kbd_feed(c);
        }
    }
    for (int i = 0; i < 6; i++)
        kbd_prev_keys[i] = cur[i];
    kbd_prev_mod = mod;
}

/* ---------- 键盘端点：投递 / 收割 ---------- */
static void kbd_arm(void)
{
    struct trb *t = &kbd_ring[kbd_enq];
    t->param = (u64)kbd_buf;
    t->status = 8;
    t->control = (TR_NORMAL << TRB_TYPE_SH) | TRB_IOC |
                 (kbd_cycle ? TRB_C : 0);
    kbd_enq++;
    if (kbd_enq == NKBD_TRB - 1) {
        struct trb *l = &kbd_ring[NKBD_TRB - 1];
        l->param = (u64)&kbd_ring[0];
        l->status = 0;
        l->control = (TR_LINK << TRB_TYPE_SH) | (1u << 1) |
                     (kbd_cycle ? TRB_C : 0);
        kbd_enq = 0;
        kbd_cycle ^= 1;
    }
    ep_doorbell(kbd_ep_dci);
}

/* ---------- 公开接口 ---------- */
void usb_kbd_poll(void)
{
    if (!kbd_ready)
        return;

    for (;;) {
        u32 type, cc, slot, ep;
        u64 param;
        if (!evt_poll_one(&type, &cc, &slot, &ep, &param))
            break;
        if (type == ER_TRANSFER && (int)ep == kbd_ep_dci) {
            if (cc == CC_SUCCESS || cc == CC_SHORT_PKT) {
                handle_report(kbd_buf);
            }
            kbd_arm();               /* 重新投递下一个请求 */
        }
    }
}

int usb_kbd_init(void)
{
    int bus, dev, fn;

    kbd_ready = 0;

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
    erst[0] = (u64)evt_ring;                       /* 0-7: 段基址 */
    /* ERST 条目第 2 个 8 字节：规范把"段大小"放在偏移 12（前 4 字节保留），
       但 QEMU 的 XHCIEvRingSeg 是 {addr_lo,addr_hi,size,rsvd} —— size 在偏移 8！
       两边都填，QEMU 和真机都能认。 */
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

    /* 9. 遍历所有「有设备」的端口：复位 → 枚举 → 找 HID boot 键盘
       ⚠️ 一台虚拟机常挂多个 USB 设备（usb-tablet / usb-mouse / usb-kbd）。
          只认「第一个有设备的端口」会抓错设备（2026-10-24 真机踩过：抓到 usb-tablet，
          报「没找到 HID boot 键盘接口」直接放弃 → 用户屏幕上打不了字）。
          所以逐个端口试，直到解析出 HID boot 键盘接口为止。 */
    int port = -1, speed = 0, found = 0;
    int ep_num = 0;
    u16 maxpkt = 0;
    u8 interval = 0;
    u32 cc = 0, sl = 0;
    int total = 0;
    struct usb_dev_desc dd;
    u8 cfg_head[9];
    struct usb_conf_desc *cd = (struct usb_conf_desc *)cfg_head;

    for (int p = 1; p <= maxports && !found; p++) {
        u32 ps = rd32((volatile u32 *)((u8 *)xhci_op + XOP_PORTSC(p)));
        if (!(ps & PORTSC_CCS))
            continue;                          /* 该端口没设备 */
        port = p;
        put("[usb ] 端口 "); putdec(port); put(" 有设备，复位中...\n");
        if (port_reset(port, &speed) < 0) {
            put("[usb ] 端口复位失败，试下一个\n");
            continue;
        }
        put("[usb ] 端口复位完成，speed="); putdec(speed); put("\n");

        /* 10. Enable Slot */
        cmd_submit(0, 0, CR_ENABLE_SLOT << TRB_TYPE_SH);
        cc = 0; sl = 0;
        if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
            put("[usb ] Enable Slot 失败 cc="); puthex(cc); put("\n");
            put("[dbg ] CRCR="); puthex(rd32(xhci_op + XOP_CRCR / 4));
            put(" cfg lo="); puthex((u32)(u64)cmd_ring);
            put(" USBSTS="); puthex(rd32(xhci_op + XOP_USBSTS / 4));
            put(" ERSTSZ="); puthex(rd32((volatile u32 *)((u8 *)xhci_rt + XRT_ERSTSZ(0))));
            put(" ERSTBA="); puthex(rd32((volatile u32 *)((u8 *)xhci_rt + XRT_ERSTBA(0))));
            put(" ERDP="); puthex(rd32((volatile u32 *)((u8 *)xhci_rt + XRT_ERDP(0))));
            put("\n[dbg ] evt0 ctl="); puthex(evt_ring[0].control);
            put(" st="); puthex(evt_ring[0].status);
            put(" cmdtrb ctl="); puthex(cmd_ring[0].control);
            put("\n");
            continue;
        }
        slot_id = sl;
        put("[usb ] slot="); putdec(slot_id); put("\n");

        /* 11. Address Device */
        in_ctx_zero();
        set_slot_ctx(1, port, speed);
        /* EP0 环：先清空，cycle=1，dequeue 指向环首 */
        for (int i = 0; i < NEP0_TRB; i++) {
            ep0_ring[i].param = 0; ep0_ring[i].status = 0; ep0_ring[i].control = 0;
        }
        ep0_enq = 0;
        ep0_cycle = 1;
        ep0_ring[NEP0_TRB - 1].param = (u64)&ep0_ring[0];
        ep0_ring[NEP0_TRB - 1].control = (TR_LINK << TRB_TYPE_SH) | (1u << 1);
        set_ep_ctx(1, EP_TYPE_CTRL, 8, 0, (u64)ep0_ring, 1);

        /* add_flags 必须 = 0x3（slot + EP0），QEMU 硬校验 */
        ((volatile u32 *)in_ctx)[1] = CTX_ADD_SLOT | CTX_ADD_EP0;

        /* 设备上下文地址登记 */
        dcbaa[slot_id] = (u64)dev_ctx;

        cmd_submit((u64)in_ctx, 0, CR_ADDRESS_DEV << TRB_TYPE_SH |
                    ((u32)slot_id << TRB_SLOTID_SH));
        if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
            put("[usb ] Address Device 失败 cc="); puthex(cc); put("\n");
            continue;
        }
        put("[usb ] 设备已寻址\n");

        /* 12. 先读 8 字节设备描述符（任何设备都支持，MPS 未知时也安全），
               拿到真实 bMaxPacketSize0 后用 Evaluate Context 更新 EP0，再读完整 18 字节 */
        for (u32 i = 0; i < sizeof(dd); i++) ((u8 *)&dd)[i] = 0;
        if (get_desc(USB_DT_DEVICE, 0, 8, &dd) < 0) {
            put("[usb ] 取设备描述符(8B)失败\n");
            continue;
        }
        put("[usb ] MPS0="); putdec(dd.bMaxPacketSize0); put("\n");

        if (dd.bMaxPacketSize0 != 8) {
            in_ctx_zero();
            set_ep_ctx(1, EP_TYPE_CTRL, dd.bMaxPacketSize0, 0, (u64)ep0_ring, ep0_cycle);
            ((volatile u32 *)in_ctx)[0] = 0;                 /* drop = 0 */
            ((volatile u32 *)in_ctx)[1] = CTX_ADD_EP0;       /* 只更新 EP0 */
            cmd_submit((u64)in_ctx, 0, CR_EVAL_CONTEXT << TRB_TYPE_SH |
                        ((u32)slot_id << TRB_SLOTID_SH));
            if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
                put("[usb ] Evaluate Context 失败 cc="); puthex(cc); put("\n");
            continue;
            }
            put("[usb ] EP0 MPS 已更新为 "); putdec(dd.bMaxPacketSize0); put("\n");
        }
        if (get_desc(USB_DT_DEVICE, 0, 18, &dd) < 0) {
            put("[usb ] 取设备描述符(18B)失败\n");
            continue;
        }
        put("[usb ] 设备 "); puthex(dd.idVendor); put(":"); puthex(dd.idProduct);
        put(" 类="); putdec(dd.bDeviceClass); put("\n");

        /* 13. 取配置描述符 */
        if (get_desc(USB_DT_CONFIG, 0, 9, cfg_head) < 0) {
            put("[usb ] 取配置头失败\n");
            continue;
        }
        cd = (struct usb_conf_desc *)cfg_head;
        total = cd->wTotalLength;
        put("[usb ] 配置总长="); putdec((u32)total); put("\n");
        if (total < 9 || total > (int)sizeof(ctl_buf)) {
            put("[usb ] 配置长度异常\n");
            continue;
        }
        if (get_desc(USB_DT_CONFIG, 0, (u16)total, ctl_buf) < 0) {
            put("[usb ] 取配置失败\n");
            continue;
        }

        if (parse_config(ctl_buf, total, &ep_num, &maxpkt, &interval) < 0) {
            put("[usb ] 这个端口不是 HID boot 键盘，试下一个\n");
            continue;
        }
        found = 1;
    }   /* ← for 端口循环结束 */

    if (!found) {
        put("[usb ] 没找到 HID boot 键盘（已试过所有有设备的端口）\n");
        return -1;
    }

    kbd_ep_num = ep_num;
    kbd_maxpkt = maxpkt;
    kbd_interval = interval;
    put("[usb ] 键盘端点 0x"); puthex((u64)ep_num);
    put(" maxpkt="); putdec(maxpkt);
    put(" interval="); putdec(interval); put("\n");

    /* 14. SET_CONFIGURATION */
    if (ctrl_xfer(0x00, 9, cd->bConfigurationValue, 0, 0, 0) < 0) {
        put("[usb ] SET_CONFIGURATION 失败\n");
        return -1;
    }
    put("[usb ] 配置已设置\n");

    /* 15. HID SET_PROTOCOL(boot)；失败不致命（多数设备默认 boot） */
    if (ctrl_xfer(0x21, 0x0B, 0, 0, 0, 0) < 0)
        put("[usb ] SET_PROTOCOL 失败（忽略）\n");

    /* 16. Configure Endpoint：加键盘中断 IN 端点 */
    kbd_ep_dci = (ep_num << 1) | 1;        /* EPn IN → DCI = 2n+1 */
    in_ctx_zero();
    set_slot_ctx(kbd_ep_dci, port, speed); /* context entries = 最高 DCI */
    /* EP0 上下文也要带上（保持有效） */
    set_ep_ctx(1, EP_TYPE_CTRL, dd.bMaxPacketSize0 ? dd.bMaxPacketSize0 : 8,
               0, (u64)ep0_ring, ep0_cycle);
    /* 键盘端点环 */
    for (int i = 0; i < NKBD_TRB; i++) {
        kbd_ring[i].param = 0; kbd_ring[i].status = 0; kbd_ring[i].control = 0;
    }
    kbd_enq = 0;
    kbd_cycle = 1;
    kbd_ring[NKBD_TRB - 1].param = (u64)&kbd_ring[0];
    kbd_ring[NKBD_TRB - 1].control = (TR_LINK << TRB_TYPE_SH) | (1u << 1);

    set_ep_ctx(kbd_ep_dci, EP_TYPE_INT_IN, maxpkt, 0, (u64)kbd_ring, 1);

    /* add_flags：slot + 新端点（bit = DCI） */
    ((volatile u32 *)in_ctx)[1] = CTX_ADD_SLOT | (1u << kbd_ep_dci);

    cmd_submit((u64)in_ctx, 0, CR_CONFIG_EP << TRB_TYPE_SH |
                ((u32)slot_id << TRB_SLOTID_SH));
    if (!evt_wait_cmd(&cc, &sl) || cc != CC_SUCCESS) {
        put("[usb ] Configure Endpoint 失败 cc="); puthex(cc); put("\n");
        return -1;
    }
    put("[usb ] 键盘端点已启用 DCI="); putdec((u32)kbd_ep_dci); put("\n");

    /* 17. 投递第一个中断请求 */
    for (int i = 0; i < 6; i++) kbd_prev_keys[i] = 0;
    kbd_prev_mod = 0;
    kbd_arm();
    kbd_ready = 1;
    put("[usb ] ✅ USB 键盘就绪，可以在屏幕上打字了\n");
    return 0;
}
