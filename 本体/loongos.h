/* LoongOS 统一内核接口（kernel API v1）
 * ------------------------------------------------------------------------
 * 2026-11-09 内核优化第一轮：把原先散落在 14 个 .c 文件里的 100+ 处手写
 * `extern` 收敛到这一个头文件，作为跨模块接口的**唯一事实来源**。
 *
 * 为什么要做（为后续新功能铺路）：
 *   1) 之前同一个函数有多份声明、且不一致 —— 例如 vs_capacity_bytes()
 *      在 kernel.c 写 `unsigned long long`，在别处写 `u64`；kputs() 有三份
 *      带不同注释的声明。编译器不会跨 .c 校验，签名漂移只在链接期/运行期
 *      才暴露，加新驱动时极易踩雷。
 *   2) 新增一个驱动/命令要在多个文件里补 extern，漏一处就是隐式声明或
 *      类型不符。现在统一在这里声明，各模块只 include。
 *   3) `kputhex2` 这类只有声明、没有定义的幽灵 extern 一并清理。
 *
 * 约束：**本头文件只放声明，不放任何定义、变量、static 数据或内联函数体**，
 * 因此包含它不改变任何编译单元的生成代码（可用 kernel.elf md5 逐字节验证）。
 *
 * 基础类型：本头文件自带 typedef 并用 LOONGOS_TYPES 守卫。各 .c 里原有的
 * `typedef unsigned long u64;` 等与这里**完全同型**，C11 允许重复 typedef
 * （kernel.c 顶部注释早就说明过），因此 include 位置不受约束、不会冲突。
 *
 * 时间源陷阱（见 net.c / tcp.c）：cnt_read() / cnt_freq() 在内核构建下是
 * 各 .c 自己的 `static inline`（直接读 cntpct_el0），**不是外部符号**；
 * 只有定义 NET_NO_CNT 的宿主单元测试才把它们当 extern 注入。所以本头文件
 * 不为它们提供无条件声明，避免把 static 定义「提升」成外部符号。
 */

#ifndef LOONGOS_H
#define LOONGOS_H

#ifndef LOONGOS_TYPES
#define LOONGOS_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

/* ===================== 控制台 / 串口输出（kernel.c 提供） ===================== */
extern void kputs(const char *s);        /* 串口 + 屏幕 */
extern void kputhex(u64 v);
extern void kputdec(u32 v);
/* kputhex2 在 nvme.c / virtio_scsi.c 曾有 extern 但全内核无定义、也无调用，
   属于历史遗留幽灵声明，不再导出。需要逐字节 hex 时用 kputhex。 */

extern int  fb_query_glyph(u32 cp, const u8 **bmp, int *w, int *h, int *row_bytes);
extern void fb_console_reset(void);
extern void gfxterm_putc(char c);       /* 桌面期间捕获 uart 输出到图形终端历史 */
extern void gfx_init(void *fb, int w, int h, int stride);
extern int  gfx_screen_owned(void);

/* ===================== 输入（kernel.c / usb_kbd.c / desktop.c） ===================== */
extern void kbd_feed(char c);                       /* 带临界区塞进输入 ring */
extern void mouse_feed_abs(int ax, int ay, int btn, int wheel);
extern void mouse_feed_rel(int dx, int dy, int btn, int wheel);
extern void mouse_reset(int x, int y);
extern int  mouse_get(int *x, int *y, int *btn, int *wheel);
extern int  mouse_pop_btn(int *state);  /* P3.2 按钮边沿事件：一次状态变化 */
extern void mouse_cal_get(int *dx, int *dy);
extern void mouse_cal_set(int dx, int dy);
extern void desktop_hotkey_exit(void);             /* Ctrl+Shift+F1 */
extern int  desktop_exit_requested(void);
extern int  desktop_ring_peek_uart(void);          /* 串口让位检测 */
extern int  desktop_ring_pop_kbd(char *c);
extern void shell_dispatch_line(char *s);          /* 图形终端复用 shell 分发 */
extern void usb_kbd_poll(void);

/* ===================== 块设备（详见 blkdev.h） =====================
 * 注：vb_* 是历史遗留的「统一块 I/O 门面」，内部会转发到 virtio-scsi / nvme。
 * 新代码应优先使用 blkdev.h 的 blk_* 接口；下列声明仅为兼容既有调用点。 */
extern int  vb_init(void);
extern int  vb_present(void);
extern int  vb_modern_only(void);
extern u64  vb_capacity_bytes(void);
extern u64  vb_capacity_sectors(void);
extern int  vb_read(u64 lba, void *buf, u32 nsec);
extern int  vb_write(u64 lba, const void *buf, u32 nsec);
extern int  vb_read_block(u64 blk_no, u64 part_lba, void *buf);
extern int  vb_write_block(u64 blk_no, u64 part_lba, const void *buf);

extern int  vbl_init(void);
extern u64  vbl_capacity_bytes(void);
extern int  vbl_request(u32 type, u64 lba, void *buf, u32 nsec, int dev_writes);

extern int  vs_init(void);
extern int  vs_present(void);
extern u64  vs_capacity_bytes(void);     /* 统一为 u64；旧 kernel.c 曾写 unsigned long long */
extern int  vs_read(u64 lba, void *buf, u32 nsec);
extern int  vs_write(u64 lba, const void *buf, u32 nsec);

extern int  nv_init(void);
extern int  nv_present(void);
extern u64  nv_capacity_bytes(void);
extern int  nv_read(u64 lba, void *buf, u32 nsec);
extern int  nv_write(u64 lba, const void *buf, u32 nsec);

/* ===================== PCI 枚举（pci.c） ===================== */
extern void pci_enum(void);

/* ===================== 文件系统（fs.c，部分接口已在 fs.h） ===================== */
extern int  fs_format_disk(u64 part_lba, u32 nblocks);
extern int  fs_mount_disk(u64 part_lba, u32 nblocks);
extern int  fs_is_disk(void);
extern u32  fs_blocks(void);

/* ===================== 网络（net.c / tcp.c / virtio_net.c） ===================== */
extern int  net_init_driver(void);
extern int  net_dhcp(void);
extern int  net_static_config(const char *ip, const char *mask, const char *gw);
extern void net_ifconfig_print(void);
extern int  net_ping(const char *ip, int count);
extern void net_poll(void);
extern int  ip_send_tcp(u32 dst_ip, const u8 *seg, int len);

extern int  tcp_connect(unsigned int ip, unsigned short port, int timeout_ms);
extern int  tcp_send(const void *buf, int len);
extern int  tcp_recv(void *buf, int max, int timeout_ms);
extern void tcp_close(void);
extern int  tcp_established(void);

extern int  vn_init(void);
extern int  vn_link(void);
extern int  vn_present(void);
extern int  vn_send(const void *frame, int len);
extern int  vn_poll(int (*on_rx)(const u8 *frame, int len));
extern void vn_mac_copy(u8 out[6]);

/* ===================== 熵源（virtio_rng.c） ===================== */
extern int  rng_init(void);
extern int  rng_present(void);
extern int  rng_get(void *buf, u32 len);

/* ===================== SMP / 时钟（smp.c / kernel.c） ===================== */
extern int  smp_start(unsigned long *cpu_mask);
extern void smp_set_gic_ver(int v);
extern int  smp_wait_online(unsigned long cpu, int spins);
extern unsigned long smp_cpu_id(void);
extern void smp_tick_inc(void);
extern void smp_secondary_entry(void);
extern volatile unsigned long smp_online[4];
extern volatile unsigned long smp_counter[4];

/* 多核工作分派（阶段9 正式版：把真实工作分派到从核，而非空转计数） */
#define SMP_JOB_CHK  1
#define SMP_JOB_FILL 2
struct smp_job {
    volatile int  active;     /* 主核写 1 表示有活；从核看到才认领 */
    volatile int  type;       /* SMP_JOB_CHK / SMP_JOB_FILL */
    volatile const u8 *buf;
    volatile u64  len;
    volatile int  ncores;     /* 参与核数（含主核 CPU0） */
    volatile u32  iters;      /* 单分区叠算遍数（放大计算量） */
    volatile u32  part[4];    /* 各核部分结果 */
    volatile u64  cycles[4];  /* 各核耗时（cntpct 周期） */
    volatile int  done;       /* 完成核计数（原子递增） */
    volatile u8   done_by[4]; /* 每核完成标志（各自写自己的字节，无跨核 RMW 竞争） */
    volatile int  fallback;   /* 1 = 有核未唤醒，主核已补算（降级单核） */
    volatile u64  seq;        /* 任务序号，从核凭它判断是否已经做过本轮 */
};
extern struct smp_job G_job;
extern void smp_do_partition(int cpu);
extern int  smp_job_run(int type, u32 iters, int ncores_limit);
extern void smp_job_compute_seq(int type, u32 iters, int ncores_limit);
extern int  smp_online_count(void);
extern volatile unsigned long smp_jobs_done[4];
extern volatile unsigned long smp_work_cycles[4];
extern volatile unsigned long smp_ipi_hits[4];
extern volatile int smp_no_conduit;
extern u32  smp_combine(const u32 *part, int n);
#define SMP_WORK_SZ  2048       /* 多核工作缓冲大小（与 smp.c 保持一致） */

extern volatile u64 tick;                /* kernel.c 的 10Hz 全局节拍（软件，恒 10Hz） */

/* ===================== 资源调配 / 时钟（R 轮） =====================
 * timer_set_wake_hz：桌面动态调整硬件唤醒频率（tick 仍恒 10Hz）。
 * g_desktop_active：桌面态标志，抑制抢占轮转让 GUI 独占 CPU0。
 * res 命令用的诊断计数：g_wake_hz / g_timer_irqs / g_wfi_hits。 */
extern void timer_set_wake_hz(u32 hz);
extern void res_wfi_inc(void);
extern volatile u32 g_wake_hz;
extern volatile u32 g_desktop_active;
extern volatile u64 g_timer_irqs;
extern volatile u64 g_wfi_hits;

/* ===================== 链接器符号（linker.ld） =====================
 * 以数组形式声明，取地址即得段边界；不要改成标量，调用处按数组用。 */
extern u64 __text_start[],     __text_end[];
extern u64 __rodata_start[],   __rodata_end[];
extern u64 __data_start[],     __bss_end[];
extern u64 __el0text_start[],  __el0text_end[];
extern u64 __nvmedma_start[],  __nvmedma_end[];
extern u64 _stack_top[];

/* ===================== 内嵌用户程序 ELF blob（objcopy 生成） ===================== */
extern const u8 _binary_hello_elf_start[], _binary_hello_elf_end[];
extern const u8 _binary_bad_elf_start[],   _binary_bad_elf_end[];
extern const u8 _binary_hang_elf_start[],  _binary_hang_elf_end[];
extern const u8 _binary_cat_elf_start[],   _binary_cat_elf_end[];
extern const u8 _binary_args_elf_start[],  _binary_args_elf_end[];

#endif /* LOONGOS_H */
