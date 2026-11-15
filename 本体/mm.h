/* LoongOS 第五十四轮 —— 动态内存地基（演进路线 2.1）
 * ------------------------------------------------------------------------
 * 为什么要有这个文件：文档《LoongOS 演进路线：对齐 Linux/BSD》把「完全没有
 * 动态内存分配器、全静态 BSS」列为 🔴 阻塞级第一项——进程表 / 页表 / vnode /
 * socket 这些动态结构都加不进去。本模块提供：
 *
 *   1) 物理页帧位图：从一个「固定 RAM 窗口」按 4KB 页分配，窗口不占 .bss；
 *   2) kmalloc/kfree：8 档 size-class（32..4096B）空闲链表 + 大块连续页分配；
 *   3) 水位/峰值/计数：每次构建打印布局，运行期由 mmstat 查询；
 *   4) 堆完整性自检：块头 magic 检出重复释放 / 野指针释放 / 跨档错放，
 *      kmalloc_audit() 校验位图与页归属一致，kmalloc_test() 跑 1000 次零泄漏。
 *
 * 关键设计取舍（都是为了不破坏现有 53 轮积累的验收资产）：
 *   - 后端地址可注入：mm_init(void *base, u32 bytes)。内核传固定物理窗口，
 *     宿主单测传 malloc 出来的缓冲 → 「零泄漏」可以在 PC 上用 -Werror 复现，
 *     不依赖串口打印说成功（文档第八节第 3 条要求）。
 *   - 不占 BSS：控制页（位图 + 页归属表 + class 链表头）放在窗口第 0 页，
 *     BSS 里只留一个 8 字节指针。bss_budget.txt 余量不受影响。
 *   - 初始化静默：mm_init/rtc_init 不打印任何一行，golden_behavior.txt 的
 *     启动日志逐字不变。
 *
 * 这是「地基」而非「成品分配器」：不做 slab 回收 / 页归还 / 多核锁（单核串行
 * 内核，fs 也只在 shell 任务里被串行调用）。这些列在 docs 的后续轮次里。
 */
#ifndef LOONGOS_MM_H
#define LOONGOS_MM_H

#ifndef LOONGOS_MM_TYPES
#define LOONGOS_MM_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

/* 固定 RAM 窗口：QEMU virt 的 RAM 从 0x40000000 起，mmu_init 把
 * 0x40400000 之后的 1GB 全部按 2MB 大页映射成 Normal-WB（EL1 可读写），
 * 因此这里无需新增页表项。避开：内核镜像(<0x4022c000)、USER 区
 * (0x40230000-0x40400000)、ramfb(0x41000000 起 1.92MB)、UEFI GOP 帧缓冲。
 * kernel.c 的 mm_pick_window() 还会依据 bootinfo 做一次运行时避让。 */
#define MM_WIN_CAND0   0x42000000UL
#define MM_WIN_CAND1   0x44000000UL
#define MM_WIN_CAND2   0x46000000UL
#define MM_WIN_SIZE    (8UL * 1024 * 1024)   /* 8MB = 2048 页 */
#define MM_PAGE        4096u
#define MM_NCLASS      8u                    /* 32,64,...,4096 */

/* 返回 0 = 成功；-1 = 参数非法（窗口太小 / 非页对齐） */
int   mm_init(void *base, u32 bytes);
void *kmalloc(u32 n);
void  kfree(void *p);
void *kzalloc(u32 n);

/* 堆自检：返回发现的不一致数（0 = 健康） */
int   kmalloc_audit(void);
/* 1000 次确定性分配/释放 + 模式校验 + 重复释放/野指针负例；返回 0 = 全过 */
int   kmalloc_test(void);

void  mm_stat(u32 *total_pages, u32 *free_pages, u32 *used_bytes,
              u32 *peak_bytes, u32 *live_objs, u32 *alloc_cnt,
              u32 *free_cnt, u32 *fail_cnt, u32 *badfree_cnt);
u64   mm_heap_base(void);
u32   mm_heap_bytes(void);
/* 当前活跃对象字节（供 /proc/meminfo 与 res 对齐） */
u32   mm_used_bytes(void);

#endif /* LOONGOS_MM_H */
