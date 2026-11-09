/* LoongOS 块设备统一抽象层（blkdev）
 * ------------------------------------------------------------------------
 * 2026-11-09 内核优化第一轮：把「哪个磁盘驱动在用」这件事从各驱动的
 * if-else 分支里抽出来，收敛成一张驱动表 + 一组统一门面。
 *
 * 之前的问题（为后续功能铺路必须解决）：
 *   virtio_blk.c 名义上只管 virtio-blk，实际上 vb_present / vb_capacity_bytes /
 *   vb_capacity_sectors / vb_read / vb_write 五个函数里各硬编码了一段
 *   `if (!vb_ok) { if (vs_present()) ...; if (nv_present()) ...; }`。
 *   kernel.c 启动链又单独写了一遍 vb_init → vs_init → nv_init 的优先级。
 *   结果：新增一个块设备驱动（AHCI / USB 存储 / virtio 新类型）要同时改
 *   virtio_blk.c 的 5 个函数 + kernel.c 的探测链 + 类型判断，共 7 处，
 *   漏一处就出现「认得到盘但读不了」或「读的是错的盘」。
 *
 * 现在：
 *   - 每个驱动只实现并导出一个 `const struct blkdev_ops`（纯自己的逻辑，
 *     不再知道别的驱动存在）；
 *   - blk_drivers[] 是唯一的优先级表，按 virtio-blk → virtio-scsi → nvme
 *     排列（与历史完全一致，不能重排，否则行为变化）；
 *   - blk_* 门面遍历表，选中第一个 present 的驱动；新增驱动只需
 *     ① 写自己的 ops ② 在 blk_drivers[] 加一项 ③ build.sh 登记 .c，
 *     kernel.c 与各既有驱动一行都不用改；
 *   - vb_* 历史函数名保留为兼容门面（fs.c / kernel.c 大量调用点不动），
 *     内部转发到 blk_*。
 *
 * 为保证「功能不变」，抽象严格遵守三条历史语义：
 *   1) 优先级：virtio-blk > virtio-scsi > nvme；
 *   2) virtio-blk 的 capacity_sectors 用 vb_cap_sec、capacity_bytes 在 legacy
 *      下用 vbl_capacity_bytes()（这两者历史上就不一致，原样保留，不"顺手修"）；
 *   3) 探测时遇到第一个 init 成功即停止，不再初始化后续驱动。
 */

#ifndef BLKDEV_H
#define BLKDEV_H

#include "loongos.h"   /* u8/u16/u32/u64 与跨模块声明 */

struct blkdev_ops {
    const char *name;                                  /* 供 [disk] 诊断文案 */
    int  (*init)(void);                                /* 0 = 成功 */
    int  (*present)(void);                             /* 仅本驱动是否就绪 */
    u64  (*capacity_bytes)(void);
    u64  (*capacity_sectors)(void);
    int  (*read)(u64 lba, void *buf, u32 nsec);
    int  (*write)(u64 lba, const void *buf, u32 nsec);
};

#define BLK_MAX_DRIVERS 8

/* 唯一的优先级表，NULL 结尾；定义于 blkdev.c */
extern const struct blkdev_ops *const blk_drivers[];
/* 探测成功的索引（-1 = 无盘）；各驱动 init 返回值，供失败诊断打印 */
extern int blk_active;
extern int blk_init_rc[BLK_MAX_DRIVERS];

/* ---- 统一门面 ---- */
int         blk_probe_all(void);                        /* 依次 init，0=有盘 */
int         blk_present(void);
const char *blk_name(void);                             /* 当前驱动名，无盘返回 "none" */
u64         blk_capacity_bytes(void);
u64         blk_capacity_sectors(void);
int         blk_read(u64 lba, void *buf, u32 nsec);
int         blk_write(u64 lba, const void *buf, u32 nsec);
int         blk_read_block(u64 blk_no, u64 part_lba, void *buf);
int         blk_write_block(u64 blk_no, u64 part_lba, const void *buf);

/* ---- 各驱动导出的 ops（新增驱动在这里加一行 extern + 在 blkdev.c 表加一项）---- */
extern const struct blkdev_ops vb_ops;   /* virtio-blk（含 modern→legacy 回退） */
extern const struct blkdev_ops vs_ops;   /* virtio-scsi */
extern const struct blkdev_ops nv_ops;   /* NVMe 原生盘 */

/* ---- virtio-blk 驱动私有实现（供 vb_ops 使用；不再是全系统门面）----
 * 历史名 vb_read/vb_write/... 已迁到 blkdev.c 作为兼容门面。 */
int  vb_dev_present(void);
u64  vb_dev_capacity_bytes(void);
u64  vb_dev_capacity_sectors(void);
int  vb_dev_read(u64 lba, void *buf, u32 nsec);
int  vb_dev_write(u64 lba, const void *buf, u32 nsec);

#endif /* BLKDEV_H */
