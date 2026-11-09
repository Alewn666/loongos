/* LoongOS · 输入核心层（架构无关）
 * ----------------------------------------------------------------------------
 * 设计来源：Linux drivers/input/input.c + include/uapi/linux/input-event-codes.h
 *
 * 为什么重写（真机反馈「鼠标键盘逻辑是一坨」）：
 *   旧实现里，每个 HID 设备直接写全局 G_mx/G_my/G_mbtn，多个设备（tablet +
 *   相对鼠标）轮流覆盖坐标和按钮位，于是出现「点不中」「光标漂」「要点击
 *   才知道位置」「固定偏移」等一连串症状，每修一个冒出下一个。
 *
 * Linux 的做法是三层解耦，本文件照搬其语义（规模精简到裸机可用）：
 *   1) 设备层/中断上半部：只把「原始事件」input_report() 入队，不改任何状态；
 *   2) 核心层：用 EV_SYN/SYN_REPORT 把一次硬件报告封成一个**原子帧**，
 *      单生产者（中断，x86 中断不嵌套）→ SPSC 环形缓冲 → 单消费者（主循环）；
 *   3) 消费者：input_read_frame() 一次取**整帧**，帧边界处统一 apply 到
 *      「按键位图 + 指针状态」，任何时刻读到的都是一致快照，不会撕裂。
 *
 * 由此天然解决：
 *   - 按钮位覆盖：按钮是位图里的一个 bit，A 设备的 release 才清它，
 *     而不是「后到的报告 btn=0 把先到的按下冲掉」；
 *   - 修一个坏一个：状态只在帧边界变化，没有「2 秒时间窗口」这类隐式时序；
 *   - 多键/修饰键：位图查询，不依赖当前报告里恰好带着 modifier 字节。
 *
 * 本文件不依赖任何 CPU 架构、不引 kmalloc、全部静态数组，可在宿主 gcc 单测。
 */
#ifndef LOONGOS_INPUT_H
#define LOONGOS_INPUT_H

#include <stdint.h>

/* ---- 事件类型 / 编码：对齐 Linux input-event-codes.h 的语义 ---------------- */
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define EV_ABS   0x03

#define SYN_REPORT  0

#define REL_X       0x00
#define REL_Y       0x01
#define REL_WHEEL   0x08

#define ABS_X       0x00
#define ABS_Y       0x01

/* 指针按钮（沿用 Linux BTN_* 的编号区间，这里取低 3 位便于位图） */
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/* 键码：直接用 PS/2 set1 make code（0..0x7F），避免再维护一张 scancode→KEY_*
   映射表；语义与 Linux evdev 的 KEY_* 一致（数字即标识，值本身不重要）。
   修饰键单独命名，供 input_mods() 查询。 */
#define KEY_ESC     0x01
#define KEY_ENTER   0x1C
#define KEY_LCTRL   0x1D
#define KEY_LSHIFT  0x2A
#define KEY_RSHIFT  0x36
#define KEY_LALT    0x38
#define KEY_SPACE   0x39
#define KEY_CAPS    0x3A
#define KEY_F1      0x3B
#define KEY_BACK    0x0E
#define KEY_TAB     0x0F
#define KEY_UP      0x48
#define KEY_DOWN    0x50
#define KEY_LEFT    0x4B
#define KEY_RIGHT   0x4D

/* 事件值：EV_KEY 下 0=释放 1=按下 2=自动重复（Linux input.h 的 autorepeat 语义） */
#define KEY_VAL_UP    0
#define KEY_VAL_DOWN  1
#define KEY_VAL_REPEAT 2

/* 修饰键位掩码（input_mods 返回） */
#define MOD_SHIFT  0x1
#define MOD_CTRL   0x2
#define MOD_ALT    0x4
#define MOD_CAPS   0x8

/* ---- 事件结构：仿 struct input_event（去掉 timeval，用 32 位 tick） -------- */
struct input_event {
    uint32_t time;     /* 事件产生时的 tick（诊断用） */
    uint16_t type;     /* EV_* */
    uint16_t code;     /* KEY_* / REL_* / ABS_* / BTN_* */
    int32_t  value;
};

#define INPUT_FRAME_MAX 16   /* 单帧最多事件数（PS/2 一次报告 ≤ 4，留足余量） */
#define INPUT_KEY_MAX   128  /* 位图覆盖的键码上界（set1 make code < 0x80） */

/* ---- API ------------------------------------------------------------------ */

/* 初始化。abs_max：绝对设备量程（QEMU usb-tablet 为 32767），0 表示无绝对设备。
   screen_w/h：帧缓冲尺寸，用于 ABS→像素换算。 */
void input_init(int screen_w, int screen_h, int abs_max_x, int abs_max_y);

/* 生产者侧（中断上半部 / 任意单生产者上下文）：
   input_report 追加事件但不封帧；input_sync 发 SYN_REPORT 完成一帧。
   一帧内可以任意组合 REL/ABS/KEY，apply 时统一生效。 */
void input_report(uint16_t type, uint16_t code, int32_t value);
void input_sync(void);

/* 便捷：按「按钮字节边沿」上报。驱动只需传上一次和本次的按钮字节
   （bit0 左 bit1 右 bit2 中），只有变化的位才发 EV_KEY(press/release)。
   这样「A 设备没有按钮变化」就不会发 btn=0 事件去冲掉 B 设备的按下。 */
void input_report_btn_delta(int prev, int cur);

/* 滴答（PIT 100Hz 中断里调用）：推进时间 + typematic 自动重复。
   delay_ticks / rate_ticks：Linux 默认 250ms delay / 33ms repeat，
   在 100Hz 下即 25 / 4（可由调用方按实际 hz 传）。 */
void input_tick(void);
void input_set_repeat(int delay_ticks, int rate_ticks);

/* 消费者侧（主循环）：
   input_frame_ready 判断是否至少有一个完整帧；
   input_read_frame 取**一整帧**到 buf（不含 SYN），并在帧边界更新状态；
   返回 1 = 取到一帧，0 = 暂无完整帧，*n = 帧内事件数。 */
int  input_frame_ready(void);
int  input_read_frame(struct input_event *buf, int cap, int *n);

/* 帧边界后的一致状态快照 */
void input_get_pointer(int *x, int *y, int *btn);   /* btn: bit0 左 bit1 右 bit2 中 */
int  input_key_down(uint16_t code);                  /* 查按键位图 */
int  input_mods(void);                               /* MOD_* 组合 */

/* 键码 + 修饰键 → ASCII（可打印字符返回 1，功能键返回 0）。
   out[0] 存结果；不产生 UTF-8 多字节（中文输入法不在本层职责内）。 */
int  input_key_to_ascii(uint16_t code, int mods, char *out);

/* 诊断：环形缓冲溢出丢弃的事件数、已消费帧数、最近原始绝对坐标 */
int  input_lost(void);
uint32_t input_frames_consumed(void);
void input_abs_raw(int *ax, int *ay, int *seen);

#endif /* LOONGOS_INPUT_H */
