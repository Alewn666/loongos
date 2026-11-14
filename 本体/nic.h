/* LoongOS —— 多网卡抽象层（阶段10：多网卡 + 多型号 + 开机自动联网）
 *
 * 背景（用户需求）：系统里加了 8 张网卡，型号混杂
 *   Intel E1000 ×2 / Realtek RTL8139 ×2 / Intel E1000E ×2 / VirtIO ×2，
 *   但内核原来只认单张 virtio-net，且其初始化还失败了 —— 完全无法联网。
 *
 * 设计要点：
 *   1) 通用 NIC 抽象：每张卡是一个 `struct nic`（型号/PCI 位置/MAC/链路/寄存器窗口/驱动 vtable）。
 *      net.c 不再直接调 vn_*，而是对所有网卡「按活动网卡分派」。
 *   2) 多实例驱动：virtio / e1000(8254x·8257x) / rtl8139 都支持最多 NIC_MAX 张卡。
 *   3) 共享 DMA 池：内核 BSS 预算极紧（仅 8KB 余量），绝不能为 8 张卡各开一套
 *      收发环/缓冲（那样会瞬间顶穿 USER_BASE）。所以**只有当前活动网卡**才从共享池
 *      分配收发环与缓冲；切卡时重新从池里分配。池本身只一份，约 48KB。
 *   4) 开机自动联网：kernel_main 里扫卡→初始化→选一张链路 up 的卡走 DHCP。
 *
 * 注意：收发环与缓冲用「物理地址」喂给设备描述符。本内核 RAM 是 VA==PA 恒等映射，
 *      所以池里的 C 指针直接当物理地址用（见 nic.c 的 nic_pool_alloc）。
 */
#ifndef NIC_H
#define NIC_H

#include "loongos.h"

#ifndef NIC_MAX
#define NIC_MAX 8
#endif

typedef enum {
    NIC_TYPE_NONE = 0,
    NIC_TYPE_VIRTIO,
    NIC_TYPE_E1000,     /* Intel 8254x 家族（含 QEMU 的 -device e1000 = 82540EM） */
    NIC_TYPE_E1000E,    /* Intel 82574L（QEMU 的 -device e1000e，寄存器与 8257x 基本兼容） */
    NIC_TYPE_RTL8139,   /* Realtek RTL8139 */
} nic_type_t;

struct nic {
    int        present;          /* 1 = 已发现并 attach 驱动 */
    nic_type_t type;
    int        bus, dev, fn;
    u16        vendor_id;
    u16        device_id;
    const char *model;           /* "virtio" / "e1000" / "e1000e" / "rtl8139" */
    u8         mac[6];
    int        link;             /* 链路状态：1=up 0=down */
    int        slot;             /* 驱动私有状态数组下标 */
    u64        reg_base;         /* MMIO 基址（e1000/rtl8139），0=无 */
    u32        reg_io;           /* I/O 端口基址（rtl8139 IO 模式），0=无 */
    /* 驱动 vtable（attach 时按需挂上；也可由 nic.c 按 type 统一派发） */
    int  (*drv_link)(struct nic *n);
    int  (*drv_send)(struct nic *n, const u8 *frame, int len);
    int  (*drv_poll)(struct nic *n, int (*cb)(const u8 *, int));
};

/* 驱动注册表项 */
struct nic_driver {
    const char *name;
    /* 是否认领该 PCI 设备（按 vendor/device/class）；认领返回 1 */
    int  (*match)(u16 vendor, u16 device, u8 base_class);
    /* attach：发现 + 复位 + 读 MAC + 测链路（不碰 DMA 池） */
    int  (*attach)(struct nic *n);
    /* activate：从共享池分配收发环/缓冲、使能 TX/RX（切到本卡为活动卡时调用） */
    int  (*activate)(struct nic *n);
    /* link/send/poll：收发与链路查询 */
    int  (*link)(struct nic *n);
    int  (*send)(struct nic *n, const u8 *frame, int len);
    int  (*poll)(struct nic *n, int (*cb)(const u8 *, int));
};

extern struct nic  G_nics[NIC_MAX];
extern int          G_nic_count;     /* 已发现并 attach 的网卡数 */
extern int          G_nic_active;    /* 当前被网络栈使用的网卡下标（-1=无） */

/* ---- 共享 DMA 池（仅活动网卡占用）---- */
void  nic_pool_reset(void);
u64   nic_pool_alloc(u32 size, u32 align);   /* 返回物理地址（VA==PA），0=失败 */

/* ---- 扫描 / 初始化 / 选择 ---- */
void  nic_scan(void);                       /* 枚举全部网卡并 attach 驱动 */
int   nic_init_all(void);                    /* 给每张卡做 attach（幂等，scan 已做则跳过） */
int   nic_init_count(void);                  /* 成功 attach 的卡数 */

/* 选活动网卡：优先链路 up 的卡，否则 0 号；并 activate 它 */
void  nic_select_default(void);
int   nic_select(int idx);                   /* 切到 idx 号卡并 activate（返回 0/负） */

/* 活动网卡便捷接口（net.c 用） */
int   nic_active_present(void);
int   nic_active_link(void);
void  nic_active_mac(u8 out[6]);
const char *nic_active_model(void);       /* 活动网卡型号字串，无则 "?" */
int   nic_active_send(const u8 *frame, int len);
int   nic_active_poll(int (*cb)(const u8 *, int));

/* 列出全部网卡（net list） */
void  nic_list_print(void);

#endif /* NIC_H */
