/* LoongOS · 输入核心层实现（架构无关，可在宿主 gcc 单测）
 * 见 input.h 顶部的设计说明。对照 Linux：
 *   input_report  ~ input_event()/input_report_key()
 *   input_sync    ~ input_sync() 发 SYN_REPORT
 *   帧槽队列      ~ evdev client buffer（一帧满了才可读）
 *   input_tick    ~ input_repeat_key()（typematic）
 *   按键位图      ~ input_dev::key[]
 */
#include "input.h"

/* ---- 帧槽环形队列：SPSC，生产者=中断上半部，消费者=主循环 ----------------
 * 为什么用「帧槽」而不是字节流 ring：
 *   字节流 ring 里消费者需要自己找 SYN_REPORT 边界，一旦生产者写到一半被
 *   读到就会看到半个状态。帧槽把「发布」做成一次原子动作（ready=1），
 *   消费者要么看到完整帧、要么看不到，天然无撕裂。 */
#define INPUT_NFRAMES 16
struct input_frame {
    struct input_event ev[INPUT_FRAME_MAX];
    int n;
    volatile int ready;                /* 生产者发布后置 1，消费者取走后置 0 */
};
static struct input_frame G_q[INPUT_NFRAMES];
static volatile int G_prod;            /* 生产者当前槽（仅生产者改） */
static volatile int G_cons;            /* 消费者当前槽（仅消费者改） */

/* 生产者正在攒的帧，先放这里，sync 时一次性拷进槽并发布 */
static struct input_event G_pending[INPUT_FRAME_MAX];
static int G_pending_n;

/* 编译器屏障：单核裸机下 volatile + 顺序发布即可，不需要 CPU 屏障 */
#define barrier() __asm__ volatile("" ::: "memory")

/* ---- 一致状态快照（只在帧边界 apply 时更新） ------------------------------ */
static int G_px, G_py, G_pbtn;         /* 指针：x/y 像素，pbtn bit0 左 bit1 右 bit2 中 */
static uint8_t G_keybits[(INPUT_KEY_MAX + 7) / 8];  /* 按键位图 */
static int G_screen_w, G_screen_h, G_abs_max_x, G_abs_max_y;
static int G_abs_seen, G_abs_ax, G_abs_ay;         /* 诊断：最近原始绝对坐标 */

/* ---- 时间 + typematic ------------------------------------------------------ */
static uint32_t G_tick;
static uint32_t G_frames_done;
static int      G_lost;
static int      G_repeat_delay = 25;   /* 100Hz 下 250ms */
static int      G_repeat_rate  = 4;    /* 100Hz 下 40ms  */
static uint16_t G_repeat_key;          /* 0 = 没有键在重复 */
static uint32_t G_repeat_since;        /* 按下/上次重复的 tick */
static int      G_repeat_started;      /* 是否已过首次 delay */

/* ---- ASCII 映射（US layout，索引 = PS/2 set1 make code） -------------------
 * 对照 Linux drivers/tty/vt/keyboard.c 的 plain_map/shift_map。
 * 0 表示该键不产生可打印字符（功能键/修饰键），上层按 code 自行判断。 */
static const char keymap_plain[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
    [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1A]='[',[0x1B]=']',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',
    [0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
    [0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',[0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',
    [0x30]='b',[0x31]='n',[0x32]='m',
    [0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' ',
};
static const char keymap_shift[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',
    [0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',[0x0E]='\b',[0x0F]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',
    [0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',
    [0x1A]='{',[0x1B]='}',[0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',
    [0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',
    [0x27]=':',[0x28]='"',[0x29]='~',
    [0x2B]='|',[0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',
    [0x30]='B',[0x31]='N',[0x32]='M',
    [0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' ',
};

/* 是否参与 typematic（Linux：修饰键不重复，可打印键与导航键重复） */
static int is_repeatable(uint16_t c)
{
    if (c >= 128) return 0;
    return keymap_plain[c] || c == KEY_UP || c == KEY_DOWN ||
           c == KEY_LEFT || c == KEY_RIGHT;
}

static void clamp_pointer(void)
{
    if (G_screen_w > 0) {
        if (G_px < 0) G_px = 0;
        if (G_px > G_screen_w - 1) G_px = G_screen_w - 1;
    }
    if (G_screen_h > 0) {
        if (G_py < 0) G_py = 0;
        if (G_py > G_screen_h - 1) G_py = G_screen_h - 1;
    }
}

void input_init(int sw, int sh, int amaxx, int amaxy)
{
    G_screen_w = sw; G_screen_h = sh;
    G_abs_max_x = amaxx > 0 ? amaxx : 32767;
    G_abs_max_y = amaxy > 0 ? amaxy : 32767;
    G_prod = 0; G_cons = 0; G_pending_n = 0;
    G_tick = 0; G_frames_done = 0; G_lost = 0;
    G_repeat_key = 0; G_repeat_started = 0;
    G_px = sw / 2; G_py = sh / 2; G_pbtn = 0;
    G_abs_seen = 0;
    for (int i = 0; i < INPUT_NFRAMES; i++) { G_q[i].n = 0; G_q[i].ready = 0; }
    for (int i = 0; i < (int)sizeof(G_keybits); i++) G_keybits[i] = 0;
}

void input_set_repeat(int delay_ticks, int rate_ticks)
{
    if (delay_ticks > 0) G_repeat_delay = delay_ticks;
    if (rate_ticks  > 0) G_repeat_rate  = rate_ticks;
}

/* 追加一个事件到「正在攒」的帧；满了就标记丢弃（本帧仍会在 sync 时发布，
   消费者拿到的是一个完整但可能少了尾部事件的帧，绝不撕裂）。 */
void input_report(uint16_t type, uint16_t code, int32_t value)
{
    if (G_pending_n >= INPUT_FRAME_MAX) { G_lost++; return; }
    struct input_event *e = &G_pending[G_pending_n++];
    e->time = G_tick; e->type = type; e->code = code; e->value = value;

    /* typematic 在「上报」时立即启停（Linux input_handle_event 语义）：
       不等消费者读帧。否则主循环若卡一下，松键后仍会继续吐重复事件。 */
    if (type == EV_KEY && code < INPUT_KEY_MAX &&
        !(code >= BTN_LEFT && code <= BTN_MIDDLE)) {
        if (value == KEY_VAL_DOWN) {
            if (is_repeatable(code)) {
                G_repeat_key = code; G_repeat_started = 0; G_repeat_since = G_tick;
            }
        } else if (value == KEY_VAL_UP) {
            if (G_repeat_key == code) { G_repeat_key = 0; G_repeat_started = 0; }
        }
    }
}

/* 发布一帧：把 pending 整帧拷进当前槽，ready=1 是唯一发布点。 */
void input_sync(void)
{
    struct input_frame *f = &G_q[G_prod];
    if (f->ready) {                       /* 消费者还没取走：缓冲满，丢本帧 */
        G_lost += G_pending_n;
        G_pending_n = 0;
        return;
    }
    for (int i = 0; i < G_pending_n; i++) f->ev[i] = G_pending[i];
    f->n = G_pending_n;
    G_pending_n = 0;
    barrier();
    f->ready = 1;                        /* 原子发布 */
    G_prod = (G_prod + 1) % INPUT_NFRAMES;
}

/* 按钮边沿上报：驱动传「上次按钮字节」和「本次按钮字节」，只有变化的位
   才发 EV_KEY。这是根治「tablet 的 btn=0 冲掉相对鼠标按下」的关键：
   没有变化就没有事件，也就不存在覆盖。btn 字节 bit0 左 bit1 右 bit2 中。 */
void input_report_btn_delta(int prev, int cur)
{
    static const uint16_t codes[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    int diff = (prev ^ cur) & 7;
    for (int b = 0; b < 3; b++) {
        if (!(diff & (1 << b))) continue;
        input_report(EV_KEY, codes[b], (cur & (1 << b)) ? KEY_VAL_DOWN : KEY_VAL_UP);
    }
}

/* ---- 帧边界 apply：把一帧里的所有事件一次性作用到状态 -------------------- */
static void apply_frame(const struct input_event *ev, int n)
{
    for (int i = 0; i < n; i++) {
        const struct input_event *e = &ev[i];
        switch (e->type) {
        case EV_KEY: {
            uint16_t c = e->code;
            /* 按钮（0x110..）单独维护，键码（<128）维护位图 */
            if (c >= BTN_LEFT && c <= BTN_MIDDLE) {
                int bit = 1 << (c - BTN_LEFT);
                if (e->value == KEY_VAL_DOWN) G_pbtn |= bit;
                else if (e->value == KEY_VAL_UP) G_pbtn &= ~bit;
                break;
            }
            if (c >= INPUT_KEY_MAX) break;
            int byte = c >> 3, mask = 1 << (c & 7);
            if (e->value == KEY_VAL_DOWN) {
                G_keybits[byte] |= mask;
            } else if (e->value == KEY_VAL_UP) {
                G_keybits[byte] &= ~mask;
            }
            /* KEY_VAL_REPEAT 不改位图：物理状态没变，只是重复事件 */
            break;
        }
        case EV_REL:
            if (e->code == REL_X)      { G_px += e->value; clamp_pointer(); }
            else if (e->code == REL_Y) { G_py += e->value; clamp_pointer(); }
            break;
        case EV_ABS:
            if (e->code == ABS_X) {
                G_abs_ax = e->value;
                if (G_screen_w > 1)
                    G_px = (int)((long)e->value * (G_screen_w - 1) / G_abs_max_x);
                G_abs_seen = 1; clamp_pointer();
            } else if (e->code == ABS_Y) {
                G_abs_ay = e->value;
                if (G_screen_h > 1)
                    G_py = (int)((long)e->value * (G_screen_h - 1) / G_abs_max_y);
                G_abs_seen = 1; clamp_pointer();
            }
            break;
        default: break;
        }
    }
}

int input_frame_ready(void)
{
    return G_q[G_cons].ready;
}

int input_read_frame(struct input_event *buf, int cap, int *n)
{
    struct input_frame *f = &G_q[G_cons];
    if (!f->ready) { *n = 0; return 0; }
    int cnt = f->n;
    if (cnt > cap) cnt = cap;
    for (int i = 0; i < cnt; i++) buf[i] = f->ev[i];
    barrier();
    f->ready = 0;                         /* 先释放槽，再 apply（apply 可能触发新帧） */
    G_cons = (G_cons + 1) % INPUT_NFRAMES;
    apply_frame(buf, cnt);
    G_frames_done++;
    *n = cnt;
    return 1;
}

/* PIT 100Hz 中断里调用：推进 tick + typematic 自动重复。
   重复帧也走 input_report/input_sync，与设备报告共用同一生产者，安全。 */
void input_tick(void)
{
    G_tick++;
    if (!G_repeat_key) return;
    uint32_t elapsed = G_tick - G_repeat_since;
    if (!G_repeat_started) {
        if (elapsed < (uint32_t)G_repeat_delay) return;
        G_repeat_started = 1; G_repeat_since = G_tick;
    } else {
        if (elapsed < (uint32_t)G_repeat_rate) return;
        G_repeat_since = G_tick;
    }
    input_report(EV_KEY, G_repeat_key, KEY_VAL_REPEAT);
    input_sync();
}

void input_get_pointer(int *x, int *y, int *btn)
{
    if (x) *x = G_px;
    if (y) *y = G_py;
    if (btn) *btn = G_pbtn;
}

int input_key_down(uint16_t code)
{
    if (code >= INPUT_KEY_MAX) return 0;
    return (G_keybits[code >> 3] >> (code & 7)) & 1;
}

int input_mods(void)
{
    int m = 0;
    if (input_key_down(KEY_LSHIFT) || input_key_down(KEY_RSHIFT)) m |= MOD_SHIFT;
    if (input_key_down(KEY_LCTRL))  m |= MOD_CTRL;
    if (input_key_down(KEY_LALT))   m |= MOD_ALT;
    if (input_key_down(KEY_CAPS))   m |= MOD_CAPS;
    return m;
}

int input_key_to_ascii(uint16_t code, int mods, char *out)
{
    if (code >= 128) return 0;
    char c = (mods & MOD_SHIFT) ? keymap_shift[code] : keymap_plain[code];
    if (!c) return 0;
    /* CapsLock 只翻转字母，与 Linux vt 一致 */
    if ((mods & MOD_CAPS) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    /* Ctrl + 字母 → 控制字符（Ctrl+C = 0x03 之类） */
    if (mods & MOD_CTRL) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else return 0;
    }
    out[0] = c;
    return 1;
}

int  input_lost(void)              { return G_lost; }
uint32_t input_frames_consumed(void) { return G_frames_done; }
void input_abs_raw(int *ax, int *ay, int *seen)
{
    if (ax) *ax = G_abs_ax;
    if (ay) *ay = G_abs_ay;
    if (seen) *seen = G_abs_seen;
}
