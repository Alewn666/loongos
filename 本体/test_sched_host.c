/* LoongOS 58.0-B —— 调度器宿主单测（cc -std=gnu11 -Wall -Wextra -Werror）
 * 验证纯调度逻辑：五态状态机、时间片递减/到期重填、轮转公平性、yield 让出、
 * 统计准确、队列满/非法迁移负例。只测 sched.c 纯逻辑（不碰内核/硬件）。
 */
#include "sched.h"
#include <stdio.h>

static int pass = 0, fail = 0;
static void ok(int cond, const char *what)
{
    if (cond) pass++;
    else { fail++; printf("FAIL: %s\n", what); }
}
static void ok2(long got, long want, const char *what)
{
    if (got == want) pass++;
    else { fail++; printf("FAIL: %s (got=%ld want=%ld)\n", what, got, want); }
}

int main(void)
{
    struct scheduler s;

    /* 1. init：全清、current=0、统计归零 */
    sched_init(&s);
    ok2(s.current, 0u, "init current=0");
    ok2(s.next_slot, 0u, "init next_slot=0");
    ok2(s.ctx_switches, 0u, "init ctx=0");
    ok2(s.slice_expires, 0u, "init expires=0");
    ok2(s.yields, 0u, "init yields=0");
    for (u32 i = 0; i < SCHED_NPROC; i++)
        ok2(s.ent[i].state, S_FREE, "init 槽位 FREE");

    /* 2. add：分配 pid、初始 RUNNABLE、slice=满、统计清零 */
    ok2(sched_add(&s, 2), 0, "add pid2 → 槽0");
    ok2(sched_add(&s, 3), 1, "add pid3 → 槽1");
    ok2(sched_add(&s, 4), 2, "add pid4 → 槽2");
    ok2(s.ent[0].pid, 2u, "槽0 pid=2");
    ok2(s.ent[0].state, S_RUNNABLE, "add 后 RUNNABLE");
    ok2(s.ent[0].slice, SCHED_SLICE, "add 后 slice 满");
    ok2(sched_add(&s, 5), 3, "add pid5 → 槽3（表刚好满）");
    ok2(sched_add(&s, 6), -1, "表满 add 返回 -1");
    ok2(sched_add(&s, 2), -1, "重复 pid add 返回 -1");
    ok2(sched_add(&s, 0), -1, "pid=0 add 返回 -1");

    /* 3. find / 状态迁移 */
    ok(sched_find(&s, 3) == &s.ent[1], "find pid3 命中槽1");
    ok2(sched_find(&s, 99) == 0, 1, "find 不存在返回 0");
    ok2(sched_set_state(&s, 2, S_RUNNING), 0, "RUNNABLE→RUNNING 合法");
    ok2(sched_set_state(&s, 3, S_SLEEPING), 0, "RUNNABLE→SLEEPING 合法");
    ok2(sched_set_state(&s, 3, S_RUNNABLE), 0, "SLEEPING→RUNNABLE 合法");
    ok2(sched_set_state(&s, 99, S_RUNNING), -1, "不存在 pid 迁移 -1");
    ok2(sched_set_state(&s, 2, S_FREE), -1, "迁移到 S_FREE 拒绝");
    ok2(sched_set_state(&s, 2, 9), -1, "非法状态值拒绝");

    /* 4. 时间片：current 指向 RUNNING，tick 递减，减到 0 的那次即到期重填 */
    s.current = 2;
    ok2(sched_tick(&s), 0, "tick1 未到期");
    ok2(s.ent[0].slice, SCHED_SLICE - 1, "tick1 slice 减1");
    ok2(s.ent[0].ticks_run, 1u, "ticks_run=1");
    for (u32 i = 1; i < SCHED_SLICE - 1; i++) sched_tick(&s);   /* 累计 SCHED_SLICE-1 次 */
    ok2(s.ent[0].slice, 1u, "到期前 slice=1");
    ok2(sched_tick(&s), 1, "第 SCHED_SLICE 次 tick 到期返回1");
    ok2(s.ent[0].slice, SCHED_SLICE, "到期后 slice 重填");
    ok2(s.slice_expires, 1u, "到期计数=1");
    s.current = 3;                              /* pid3 是 RUNNABLE，非 RUNNING */
    ok2(sched_tick(&s), -1, "current 非 RUNNING，tick -1");
    s.current = 0;
    ok2(sched_tick(&s), -1, "无 current，tick -1");

    /* 5. 轮转公平性：4 个 RUNNABLE，连续 pick 应 0,1,2,3,0,1... 环形 */
    sched_init(&s);
    sched_add(&s, 2); sched_add(&s, 3); sched_add(&s, 4); sched_add(&s, 5);
    int seq[8];
    for (int i = 0; i < 8; i++) {
        struct sched_ent *e = sched_pick_next(&s);
        seq[i] = e ? (int)e->pid : 0;
    }
    ok2(seq[0], 2, "pick1=2");
    ok2(seq[1], 3, "pick2=3");
    ok2(seq[2], 4, "pick3=4");
    ok2(seq[3], 5, "pick4=5");
    ok2(seq[4], 2, "pick5=2（环形回绕）");
    ok2(seq[5], 3, "pick6=3");
    ok2(seq[6], 4, "pick7=4");
    ok2(seq[7], 5, "pick8=5");

    /* 6. pick 跳过非 RUNNABLE */
    sched_init(&s);
    sched_add(&s, 2); sched_add(&s, 3); sched_add(&s, 4);
    sched_set_state(&s, 3, S_SLEEPING);          /* 跳过 pid3 */
    ok2(sched_pick_next(&s)->pid, 2u, "pick 命中2");
    ok2(sched_pick_next(&s)->pid, 4u, "pick 跳过睡眠的3命中4");
    ok2(sched_pick_next(&s)->pid, 2u, "pick 环形回2");
    sched_set_state(&s, 2, S_ZOMBIE);
    sched_set_state(&s, 4, S_ZOMBIE);
    ok2(sched_pick_next(&s) == 0, 1, "全非就绪 pick 返回 0");

    /* 7. yield：当前 RUNNING 让出，切到下一个 RUNNABLE，统计准确 */
    sched_init(&s);
    sched_add(&s, 2); sched_add(&s, 3); sched_add(&s, 4);
    sched_set_state(&s, 2, S_RUNNING);           /* pid2 正在跑 */
    s.current = 2;
    ok2(sched_yield(&s), 3u, "yield 切到 pid3");
    ok2(s.ent[2].state, S_RUNNABLE, "yield 后 pid2 回 READY");
    ok2(s.ent[2].state, S_RUNNABLE, "（dup）pid2 READY");
    ok2(s.ent[1].state, S_RUNNING, "yield 后 pid3 RUNNING");
    ok2(s.current, 3u, "yield 后 current=3");
    ok2(s.yields, 1u, "yields=1");
    ok2(s.ctx_switches, 1u, "ctx_switches=1");
    ok2(s.ent[1].switches, 1u, "pid3 被切上1次");
    /* 公平轮转：pid4 从未被调度，二次 yield 必先切到它，而非回 pid2（证明轮转生效） */
    ok2(sched_yield(&s), 4u, "再 yield 切到未跑过的 pid4（轮转公平）");
    ok2(s.ent[2].state, S_RUNNING, "pid4 RUNNING");
    ok2(s.yields, 2u, "yields=2");
    ok2(sched_yield(&s), 2u, "第三次 yield 环形切回 pid2");
    ok2(s.yields, 3u, "yields=3");
    ok2(s.ctx_switches, 3u, "ctx_switches=3");
    /* 只一个 RUNNING 无其它 READY：yield 自让自回，返回0 */
    sched_init(&s);
    sched_add(&s, 2);
    sched_set_state(&s, 2, S_RUNNING);
    s.current = 2;
    ok2(sched_yield(&s), 0u, "仅一人时 yield 返回0");
    ok2(s.ent[0].state, S_RUNNING, "仅一人时保持 RUNNING");

    /* 8. remove：释放槽位、current 归零、僵尸迁移拒绝 */
    sched_init(&s);
    sched_add(&s, 2); sched_add(&s, 3);
    sched_set_state(&s, 2, S_RUNNING);
    s.current = 2;
    ok2(sched_set_state(&s, 2, S_ZOMBIE), 0, "RUNNING→ZOMBIE 合法");
    ok2(sched_set_state(&s, 2, S_RUNNABLE), -1, "僵尸不可再迁移");
    ok2(sched_remove(&s, 2), 0, "remove 僵尸成功");
    ok2(s.current, 0u, "remove 当前进程后 current=0");
    ok2(sched_find(&s, 2) == 0, 1, "remove 后 find 不到");
    ok2(s.ent[0].state, S_FREE, "remove 后槽位 FREE");
    ok2(sched_remove(&s, 2), -1, "重复 remove -1");
    ok2(sched_remove(&s, 99), -1, "remove 不存在 -1");

    printf("test_sched_host: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
