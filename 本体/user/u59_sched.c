/* 第五十八轮 58.0-B 用户态验证：抢占式调度地基（状态机/时间片/yield/统计）
 * -nostdlib EL0，经 b64 注入 /home，只编不内嵌。
 *
 * 判据（父 pid=1 先看初始统计 → fork 一子 → 子 yield 让出 → 子 procexit → 父 wait 回收）：
 *   ① 初始 schedstat：统计全 0（调度器 init 后未发生切换/让出/到期），current=0
 *      （pid=1 初始进程在调度表外）。
 *   ② 父 fork 一子 → 子登记 RUNNABLE；父 schedstat 见 1 个 RUNNABLE 实体。
 *   ③ 父 wait 切到子 → 子 state=RUNNING(2)、current=子 pid。
 *   ④ 子 yield 让出 → yields 计数增长；仅一活跃子时选回自己(ret=0)且保持 RUNNING(2)，
 *      多就绪实体时 pick_next 轮转切到别人（公平性由宿主单测 72 项覆盖）。
 *   ⑤ 子 procexit → 从调度表移除；父 wait 返回子 pid + 退出码 7。
 *   ⑥ 最终无残留实体、pid 身份不变、跨 run 零泄漏（由判据独立证明）。
 *
 * ⚠️ 只 fork 一次：合作式模型下子帧是父 fork 时的快照，多次 fork 会让子继承执行流
 *    再次 fork，产生无人回收的孤儿实体（首版两子踩过，final_live_ent=1）。
 *
 * 输出 [R59] key=value 供 test_round59.py 独立核对；sys_exit 体面退出。
 */
#include "user.h"
#include "../sched.h"

static void pc(char c) { char b[2]; b[0] = c; b[1] = 0; u_puts(b); }
static void pdec(long v)
{
    char d[24]; int k = 0;
    if (v < 0) { pc('-'); v = -v; }
    if (!v) d[k++] = '0';
    while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) pc(d[--k]);
}
static void kv(const char *k, long v)
{ u_puts("[R59] "); u_puts(k); u_puts("="); pdec(v); pc('\n'); }

static long count_live(const struct uschedstat *st)
{
    long n = 0;
    for (u32 i = 0; i < SCHED_NPROC; i++)
        if (st->ent[i].pid &&
            (st->ent[i].state == S_RUNNABLE || st->ent[i].state == S_RUNNING ||
             st->ent[i].state == S_SLEEPING || st->ent[i].state == S_ZOMBIE)) n++;
    return n;
}
static long ent_state(const struct uschedstat *st, u32 pid)
{
    for (u32 i = 0; i < SCHED_NPROC; i++)
        if (st->ent[i].pid == pid) return (long)st->ent[i].state;
    return -1;
}

int main(void)
{
    struct uschedstat st;

    kv("pid_before", u_getpid());

    /* ① 初始快照：统计全 0 */
    long r = u_schedstat(&st);
    kv("stat0_ret", r);
    kv("stat0_current", st.current);
    kv("stat0_ctx", st.ctx_switches);
    kv("stat0_yields", st.yields);
    kv("stat0_expires", st.slice_expires);
    kv("stat0_live", count_live(&st));

    /* ② fork 一子（严格单次，子用 pid==0 分流） */
    long pid = u_fork();
    if (pid != 0) kv("fork_ret", pid);

    if (pid == 0) {
        /* ---- 子：wait 切来后是 RUNNING，读快照 → yield → procexit ---- */
        long me = u_getpid();
        kv("child_pid", me);
        u_schedstat(&st);
        kv("child_state_before", ent_state(&st, (u32)me));
        kv("child_current_before", st.current);
        long yr = u_yield();                        /* 主动让出 */
        kv("child_yield_ret", yr);
        u_schedstat(&st);
        kv("child_yields_global", st.yields);
        kv("child_ctx_global", st.ctx_switches);
        kv("child_state_after", ent_state(&st, (u32)me));
        u_procexit(7);                              /* 退出，切回父 */
        for (;;) {}
    }

    /* ---- 父：回收子，检查清理 ---- */
    if (pid < 0) { kv("fail", 1); sys_exit(1); }
    int status = -1;
    long w = u_wait(&status);
    kv("wait_ret", w);
    kv("status", status);
    u_schedstat(&st);
    kv("final_live", count_live(&st));
    kv("final_yields", st.yields);
    kv("final_ctx", st.ctx_switches);
    kv("pid_after", u_getpid());

    int pass = 0, fail = 0;
    #define CHK(c) do { if (c) pass++; else fail++; } while (0)
    CHK(r == 0);
    CHK(pid >= 2);
    CHK(w == pid);
    CHK(status == 7);
    CHK(u_getpid() == 1);
    CHK(count_live(&st) == 0);
    kv("pass", pass);
    kv("fail", fail);
    sys_exit(fail ? 1 : 0);
    return 0;
}
