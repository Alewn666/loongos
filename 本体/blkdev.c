/* LoongOS 块设备统一抽象层实现
 * ------------------------------------------------------------------------
 * 设计要点见 blkdev.h。这里只做三件事：
 *   1) 定义唯一的优先级表 blk_drivers[]；
 *   2) blk_probe_all() 依次 init，遇第一个成功即停（与历史启动链一致）；
 *   3) blk_* 门面按「第一个 present 的驱动」分发，而不是按探测索引 ——
 *      历史 vb_* 正是这么做的（运行时判 vb_ok/vs_present/nv_present），
 *      cmd_scsi 可能单独把 scsi 初始化起来，按索引会读错盘。
 *
 * 新增块设备驱动的步骤（今后不用再改 kernel.c / 其它驱动）：
 *   ① 新驱动 .c 里实现 init/present/read/write/capacity_*，导出 xxx_ops；
 *   ② 在 blkdev.h 加一行 extern、在下面 blk_drivers[] 加一项；
 *   ③ 在 build.sh 的 KERN_SRCS 登记该 .c（未登记会被守卫拦下）。
 */

#include "blkdev.h"

/* 优先级顺序 = 历史 vb_init → vs_init → nv_init，不可重排 */
const struct blkdev_ops *const blk_drivers[] = {
    &vb_ops,
    &vs_ops,
    &nv_ops,
    0
};

/* ⚠️ 这两个放 .data 且显式初始化，绝不能放 .bss：
 * .bss 里每个驱动队列都是 4KB 对齐（aligned(4096)），一个 32 字节的变量
 * 落在对齐缝隙里就可能把后续整条段链推过 4KB 边界，实测会让 .nvmedma
 * 起始地址后移一整页、USER_BASE 余量从 8KB 掉到 4KB。给非零初值可让
 * 编译器把它们放进 .data，避免挤占 .bss 的对齐布局。
 * 初值 -1 语义：尚未探测，失败诊断打印时不会误用 0。 */
int blk_active = -1;                                  /* -1 = 无盘 */
int blk_init_rc[BLK_MAX_DRIVERS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

/* 选中「第一个 present 的驱动」——严格复现历史 vb_* 的运行时优先级 */
static const struct blkdev_ops *blk_find(void)
{
    for (int i = 0; blk_drivers[i]; i++)
        if (blk_drivers[i]->present())
            return blk_drivers[i];
    return 0;
}

int blk_probe_all(void)
{
    blk_active = -1;
    for (int i = 0; i < BLK_MAX_DRIVERS && blk_drivers[i]; i++) {
        int r = blk_drivers[i]->init();
        blk_init_rc[i] = r;
        if (r == 0) {              /* 与历史一致：第一个成功即停，不初始化后续驱动 */
            blk_active = i;
            return 0;
        }
    }
    return -1;
}

int blk_present(void)         { return blk_find() != 0; }
const char *blk_name(void)
{
    const struct blkdev_ops *d = blk_find();
    return d ? d->name : "none";
}
u64 blk_capacity_bytes(void)
{
    const struct blkdev_ops *d = blk_find();
    return d ? d->capacity_bytes() : 0;
}
u64 blk_capacity_sectors(void)
{
    const struct blkdev_ops *d = blk_find();
    return d ? d->capacity_sectors() : 0;
}
int blk_read(u64 lba, void *buf, u32 nsec)
{
    const struct blkdev_ops *d = blk_find();
    return d ? d->read(lba, buf, nsec) : -1;
}
int blk_write(u64 lba, const void *buf, u32 nsec)
{
    const struct blkdev_ops *d = blk_find();
    return d ? d->write(lba, buf, nsec) : -1;
}
int blk_read_block(u64 blk_no, u64 part_lba, void *buf)
{
    return blk_read(part_lba + blk_no * 8, buf, 8);   /* fs 层一块 = 4KB = 8 扇区 */
}
int blk_write_block(u64 blk_no, u64 part_lba, const void *buf)
{
    return blk_write(part_lba + blk_no * 8, buf, 8);
}

/* ------------------------------------------------------------------------
 * 历史兼容门面：vb_* 这些名字被 fs.c / kernel.c / install 大量调用，
 * 保留原名（定义在 loongos.h），内部只做一次转发到 blk_*。
 * 这样既有调用点一行都不用改，且分发逻辑只有上面这一份。
 * ---------------------------------------------------------------------- */
int  vb_present(void)          { return blk_present(); }
u64  vb_capacity_bytes(void)   { return blk_capacity_bytes(); }
u64  vb_capacity_sectors(void) { return blk_capacity_sectors(); }
int  vb_read(u64 lba, void *buf, u32 nsec)              { return blk_read(lba, buf, nsec); }
int  vb_write(u64 lba, const void *buf, u32 nsec)        { return blk_write(lba, buf, nsec); }
int  vb_read_block(u64 blk_no, u64 part_lba, void *buf)  { return blk_read_block(blk_no, part_lba, buf); }
int  vb_write_block(u64 blk_no, u64 part_lba, const void *buf)
                                                         { return blk_write_block(blk_no, part_lba, buf); }
