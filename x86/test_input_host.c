/* LoongOS · 输入核心层宿主单测（架构无关，gcc 直接编译运行）
 * 目的：在进 QEMU 之前把「帧原子性 / 按钮不覆盖 / 修饰键 / typematic」跑对。
 * 编译：gcc -O2 -Wall -o /tmp/ti test_input_host.c input.c && /tmp/ti
 */
#include <stdio.h>
#include <string.h>
#include "input.h"

static int T, PASS, FAIL;
#define OK(cond, name) do { T++; if (cond) { PASS++; printf("  [PASS] %s\n", name); } \
    else { FAIL++; printf("  [FAIL] %s  (line %d)\n", name, __LINE__); } } while (0)

/* 模拟一个「相对鼠标」设备：一帧里 dx/dy + 按钮字节边沿 */
static void feed_rel_mouse(int dx, int dy, int *prev_btn, int cur_btn)
{
    if (dx) input_report(EV_REL, REL_X, dx);
    if (dy) input_report(EV_REL, REL_Y, dy);
    input_report_btn_delta(*prev_btn, cur_btn);
    *prev_btn = cur_btn;
    input_sync();
}

/* 模拟一个「绝对平板」设备：一帧里 ax/ay + 按钮字节边沿 */
static void feed_abs_tablet(int ax, int ay, int *prev_btn, int cur_btn)
{
    input_report(EV_ABS, ABS_X, ax);
    input_report(EV_ABS, ABS_Y, ay);
    input_report_btn_delta(*prev_btn, cur_btn);
    *prev_btn = cur_btn;
    input_sync();
}

static int drain_frames(struct input_event *all, int cap)
{
    struct input_event buf[INPUT_FRAME_MAX];
    int n, total = 0;
    while (input_frame_ready()) {
        if (!input_read_frame(buf, INPUT_FRAME_MAX, &n)) break;
        for (int i = 0; i < n && total < cap; i++) all[total++] = buf[i];
    }
    return total;
}

/* 模拟真实主循环：把所有已发布的帧消费掉，让状态推进到最新帧边界 */
static void consume(void)
{
    struct input_event buf[INPUT_FRAME_MAX];
    int n;
    while (input_frame_ready()) input_read_frame(buf, INPUT_FRAME_MAX, &n);
}

int main(void)
{
    printf("=== LoongOS 输入核心层单测（仿 Linux input 模型）===\n\n");

    /* ---------- 1. 帧原子性 ---------- */
    printf("[1] 帧原子性 / SPSC 队列\n");
    input_init(800, 600, 32767, 32767);
    input_report(EV_REL, REL_X, 10);
    input_report(EV_REL, REL_Y, 20);
    /* 还没 sync：消费者绝不能看到半个帧 */
    OK(!input_frame_ready(), "攒帧期间消费者看不到半帧");
    input_sync();
    OK(input_frame_ready(), "sync 后才有完整帧");
    {
        struct input_event buf[INPUT_FRAME_MAX]; int n;
        OK(input_read_frame(buf, INPUT_FRAME_MAX, &n) == 1 && n == 2,
           "取到整帧且事件数=2（不含 SYN）");
        OK(!input_frame_ready(), "取走后队列为空");
        int x, y, b; input_get_pointer(&x, &y, &b);
        OK(x == 410 && y == 320, "帧边界统一 apply：相对位移累加到 (410,320)");
    }

    /* ---------- 2. 多设备按钮不互相覆盖（旧版真正的 bug） ---------- */
    printf("\n[2] tablet + 相对鼠标共存，按钮位不被冲掉\n");
    input_init(800, 600, 32767, 32767);
    {
        int rel_prev = 0, abs_prev = 0;
        /* 相对鼠标按下左键（btn=1） */
        feed_rel_mouse(0, 0, &rel_prev, 1);
        /* 平板移动但没按任何键（btn=0）——旧实现会用这个 0 冲掉按下位 */
        feed_abs_tablet(16383, 16383, &abs_prev, 0);
        struct input_event all[64]; int total = drain_frames(all, 64);
        int x, y, b; input_get_pointer(&x, &y, &b);
        OK((b & 1) == 1, "平板 btn=0 报告后，左键仍保持按下（未被冲掉）");
        OK(x == 399 && y == 299, "平板绝对坐标正确换算到屏幕中心");
        /* 相对鼠标松开左键 */
        feed_rel_mouse(0, 0, &rel_prev, 0);
        drain_frames(all, 64);
        input_get_pointer(&x, &y, &b);
        OK((b & 1) == 0, "释放事件到达后左键才抬起");
        (void)total;
    }

    /* ---------- 3. 只发边沿：无变化时零事件 ---------- */
    printf("\n[3] 按钮只在边沿发事件\n");
    input_init(800, 600, 32767, 32767);
    {
        int prev = 0;
        feed_rel_mouse(0, 0, &prev, 0);   /* 无位移、无按钮变化 */
        struct input_event buf[INPUT_FRAME_MAX]; int n;
        input_read_frame(buf, INPUT_FRAME_MAX, &n);
        OK(n == 0, "空帧里 0 个事件（不产生垃圾 press/release）");
    }

    /* ---------- 4. 修饰键状态机（位图，不依赖当前报告） ---------- */
    printf("\n[4] 修饰键位图 + Ctrl+Shift+F1 组合键\n");
    input_init(800, 600, 32767, 32767);
    {
        input_report(EV_KEY, KEY_LCTRL, 1);  input_sync();
        input_report(EV_KEY, KEY_LSHIFT, 1); input_sync();
        input_report(EV_KEY, KEY_F1, 1);     input_sync();  /* 关键帧 */
        consume();                                          /* 主循环消费，位图推进 */
        int mods = input_mods();
        OK((mods & MOD_CTRL) && (mods & MOD_SHIFT), "Ctrl+Shift 同时按下可从位图查出");
        char c;
        OK(input_key_to_ascii(KEY_F1, mods, &c) == 0, "F1 不是可打印字符");
        /* 松开 Shift 后只剩 Ctrl */
        input_report(EV_KEY, KEY_LSHIFT, 0); input_sync();
        consume();
        mods = input_mods();
        OK((mods & MOD_CTRL) && !(mods & MOD_SHIFT), "松 Shift 后修饰键状态正确更新");
    }

    /* ---------- 5. ASCII 映射 / Shift / Ctrl ---------- */
    printf("\n[5] 键码 → ASCII\n");
    input_init(800, 600, 32767, 32767);
    {
        char c;
        OK(input_key_to_ascii(0x1E, 0, &c) == 1 && c == 'a', "A 键 → 'a'");
        OK(input_key_to_ascii(0x1E, MOD_SHIFT, &c) == 1 && c == 'A', "Shift+A → 'A'");
        OK(input_key_to_ascii(0x1E, MOD_CTRL, &c) == 1 && c == 1, "Ctrl+A → 0x01");
        OK(input_key_to_ascii(0x1C, 0, &c) == 1 && c == '\n', "Enter → 换行");
        OK(input_key_to_ascii(0x0E, 0, &c) == 1 && c == '\b', "Backspace → 退格");
        OK(input_key_to_ascii(0x02, MOD_SHIFT, &c) == 1 && c == '!', "Shift+1 → '!'");
    }

    /* ---------- 6. typematic 自动重复（Linux value=2 语义） ---------- */
    printf("\n[6] typematic 自动重复\n");
    input_init(800, 600, 32767, 32767);
    input_set_repeat(5, 2);   /* 加速测试：5 tick delay，2 tick rate */
    {
        input_report(EV_KEY, 0x1E, 1); input_sync();   /* 按住 'a' */
        /* 前 4 tick 不该重复 */
        for (int i = 0; i < 4; i++) input_tick();
        int rep_before = 0;
        {
            struct input_event buf[INPUT_FRAME_MAX]; int n;
            while (input_frame_ready()) {
                input_read_frame(buf, INPUT_FRAME_MAX, &n);
                for (int j = 0; j < n; j++)
                    if (buf[j].type == EV_KEY && buf[j].value == KEY_VAL_REPEAT) rep_before++;
            }
        }
        OK(rep_before == 0, "delay 未到期不重复");
        /* 继续推进，应该出现 repeat 事件 */
        for (int i = 0; i < 12; i++) input_tick();
        int rep_after = 0, repeats_for_a = 0;
        {
            struct input_event buf[INPUT_FRAME_MAX]; int n;
            while (input_frame_ready()) {
                input_read_frame(buf, INPUT_FRAME_MAX, &n);
                for (int j = 0; j < n; j++)
                    if (buf[j].type == EV_KEY && buf[j].value == KEY_VAL_REPEAT) {
                        rep_after++;
                        if (buf[j].code == 0x1E) repeats_for_a++;
                    }
            }
        }
        OK(rep_after > 0, "delay 到期后产生自动重复帧");
        OK(repeats_for_a == rep_after, "重复事件键码正确");
        /* 位图不受 repeat 影响（物理状态仍是按下） */
        OK(input_key_down(0x1E), "repeat 不改按键位图，仍为按下");
        /* 松开后停止重复 */
        input_report(EV_KEY, 0x1E, 0); input_sync();
        for (int i = 0; i < 20; i++) input_tick();
        int rep_stop = 0;
        {
            struct input_event buf[INPUT_FRAME_MAX]; int n;
            while (input_frame_ready()) {
                input_read_frame(buf, INPUT_FRAME_MAX, &n);
                for (int j = 0; j < n; j++)
                    if (buf[j].type == EV_KEY && buf[j].value == KEY_VAL_REPEAT) rep_stop++;
            }
        }
        OK(rep_stop == 0, "松开后不再重复");
        OK(!input_key_down(0x1E), "松开后位图清零");
    }

    /* ---------- 7. 相对/绝对统一到同一指针状态 ---------- */
    printf("\n[7] 相对/绝对坐标统一归约\n");
    input_init(800, 600, 32767, 32767);
    {
        int pb = 0;
        feed_abs_tablet(0, 0, &pb, 0);          /* 绝对到左上 */
        consume();
        int x, y, b; input_get_pointer(&x, &y, &b);
        OK(x == 0 && y == 0, "ABS(0,0) → (0,0)");
        feed_rel_mouse(50, 30, &pb, 0);          /* 相对移动跟随 */
        consume();
        input_get_pointer(&x, &y, &b);
        OK(x == 50 && y == 30, "绝对之后相对位移仍能实时跟随（P2.2 回归点）");
        feed_abs_tablet(32767, 32767, &pb, 0);  /* 绝对到右下 */
        consume();
        input_get_pointer(&x, &y, &b);
        OK(x == 799 && y == 599, "ABS(max) → 右下角，clamp 正确");
    }

    /* ---------- 8. 队列溢出不撕裂 ---------- */
    printf("\n[8] 帧队列溢出保护\n");
    input_init(800, 600, 32767, 32767);
    {
        for (int i = 0; i < 100; i++) {         /* 远超过 16 帧槽，且不消费 */
            input_report(EV_REL, REL_X, 1);
            input_sync();
        }
        OK(input_lost() > 0, "溢出被计数（不静默丢数据）");
        /* 消费时每帧仍完整，绝不出现半帧 */
        struct input_event buf[INPUT_FRAME_MAX]; int n, frames = 0, bad = 0;
        while (input_frame_ready()) {
            if (!input_read_frame(buf, INPUT_FRAME_MAX, &n)) break;
            frames++;
            if (n > INPUT_FRAME_MAX) bad++;
        }
        OK(frames > 0 && frames <= 16, "溢出后仍能消费完整帧且数量≤槽数");
        OK(bad == 0, "没有帧超过单帧上限（无撕裂/越界）");
        OK(input_frames_consumed() == (uint32_t)frames, "消费帧计数一致");
    }

    printf("\n=== 结果：%d 项，%d 通过，%d 失败 ===\n", T, PASS, FAIL);
    return FAIL ? 1 : 0;
}
