/* LoongOS 第五十四轮 —— 真实墙钟 RTC（演进路线 2.2）
 * ------------------------------------------------------------------------
 * 文档原话：「现在 ls -l 时间是『开机第几秒』，这是『不像 OS』的第一眼破绽」。
 * 本模块读 QEMU virt 的 PL031 RTC（MMIO 0x09010000，DR 寄存器 = Unix 秒），
 * 提供年月日时分秒 + 星期 + 时区偏移。选择 PL031 而非 UEFI RuntimeServices
 * GetTime() 的原因：后者需要把 SystemTable 指针从 stub 一路传进内核、跨
 * ExitBootServices 后还可能被回收；PL031 已经在 mmu_init 的 l2_dev[72]
 * （0x09000000 起 2MB 设备块）映射内，零页表改动、零启动流程改动。
 *
 * 实测（本轮 QMP xp/4xw 0x9010000）：DR=0x6af9af0d → 1794748173，隔 1s
 * 变 0x6af9af0e → 1794748174，与宿主 time.time() 逐秒对齐。
 *
 * 保基线：rtc_init() 静默、date 命令刻意不进 help / golden_behavior.txt。
 */
#ifndef LOONGOS_RTC_H
#define LOONGOS_RTC_H

#ifndef LOONGOS_RTC_TYPES
#define LOONGOS_RTC_TYPES
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
#endif

#define RTC_BASE   0x09010000UL
#define RTC_DR     (*(volatile u32 *)(RTC_BASE + 0x00))  /* Data: Unix 秒 */
#define RTC_CR     (*(volatile u32 *)(RTC_BASE + 0x0C))  /* Control */

void  rtc_init(void);                 /* 静默探测，保行为基线 */
u64   rtc_now(void);                  /* 当前 Unix 秒（UTC）；不可用返回 0 */
int   rtc_valid(void);                /* 1 = PL031 读数可信 */
void  rtc_set_tz(int minutes);        /* 时区偏移（分钟），默认 +480 = UTC+8 */
int   rtc_tz(void);
/* 格式化 "YYYY-MM-DD HH:MM:SS Www UTC+HHMM"，out 至少 48B */
void  rtc_format(u64 epoch, char *out, u32 cap);
/* 拆分字段（供 fs 存墙钟 / 单测逐字段校验） */
void  rtc_breakdown(u64 epoch, int *y, int *m, int *d,
                    int *hh, int *mm, int *ss, int *wday);
u64   rtc_epoch_of(int y, int m, int d, int hh, int mm, int ss); /* 逆变换，单测用 */

#endif /* LOONGOS_RTC_H */
