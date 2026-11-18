/* LoongOS 58.0-B —— 调度器纯逻辑（见 sched.h 的设计取舍）
 * freestanding；只用自有循环，不依赖 libc / MMIO / kmalloc，宿主可 -Werror 编译。
 */
#include "sched.h"

static void szero(void *p, u32 n)
{
    u8 *b = (u8 *)p;
    for (u32 i = 0; i < n; i++) b[i] = 0;
}

void sched_init(struct scheduler *s)
{
    if (!s) return;
    szero(s, sizeof *s);
}

struct sched_ent *sched_find(struct scheduler *s, u32 pid)
{
    if (!s || pid == 0) return 0;
    for (u32 i = 0; i < SCHED_NPROC; i++)
        if (s->ent[i].state != S_FREE && s->ent[i].pid == pid)
            return &s->ent[i];
    return 0;
}

int sched_add(struct scheduler *s, u32 pid)
{
    if (!s || pid == 0) return -1;
    if (sched_find(s, pid)) return -1;              /* 重复 pid 拒绝 */
    for (u32 i = 0; i < SCHED_NPROC; i++) {
        if (s->ent[i].state == S_FREE) {
            s->ent[i].pid = pid;
            s->ent[i].state = S_RUNNABLE;
            s->ent[i].slice = SCHED_SLICE;
            s->ent[i].ticks_run = 0;
            s->ent[i].switches = 0;
            return (int)i;
        }
    }
    return -1;                                      /* 表满 */
}

int sched_remove(struct scheduler *s, u32 pid)
{
    struct sched_ent *e = sched_find(s, pid);
    if (!e) return -1;
    if (s->current == pid) s->current = 0;
    e->state = S_FREE;
    e->pid = 0;
    return 0;
}

int sched_set_state(struct scheduler *s, u32 pid, u32 st)
{
    struct sched_ent *e = sched_find(s, pid);
    if (!e) return -1;
    if (st == S_FREE) return -1;                    /* 释放只能走 sched_remove */
    if (st > S_ZOMBIE) return -1;                   /* 非法状态值 */
    if (e->state == S_ZOMBIE) return -1;            /* 僵尸不可再迁移，只能 remove */
    e->state = st;
    return 0;
}

struct sched_ent *sched_pick_next(struct scheduler *s)
{
    if (!s) return 0;
    for (u32 k = 0; k < SCHED_NPROC; k++) {
        u32 i = (s->next_slot + k) % SCHED_NPROC;
        if (s->ent[i].state == S_RUNNABLE) {
            s->next_slot = (i + 1) % SCHED_NPROC;   /* 下次从下一个槽开始 → 轮转公平 */
            return &s->ent[i];
        }
    }
    return 0;
}

int sched_tick(struct scheduler *s)
{
    if (!s || s->current == 0) return -1;
    struct sched_ent *e = sched_find(s, s->current);
    if (!e || e->state != S_RUNNING) return -1;
    e->ticks_run++;
    if (e->slice > 0) e->slice--;
    if (e->slice == 0) {
        e->slice = SCHED_SLICE;
        s->slice_expires++;
        return 1;
    }
    return 0;
}

u32 sched_yield(struct scheduler *s)
{
    if (!s || s->current == 0) return 0;
    struct sched_ent *cur = sched_find(s, s->current);
    if (!cur || cur->state != S_RUNNING) return 0;
    u32 slot = (u32)(cur - s->ent);                 /* 当前进程所在槽号 */
    cur->state = S_RUNNABLE;                        /* 放弃 CPU，回到就绪队列 */
    s->yields++;
    /* 从自己的下一个槽开始选：否则 next_slot 若正指向本槽会重新选中自己（实测抓到）。 */
    s->next_slot = (slot + 1) % SCHED_NPROC;
    struct sched_ent *nxt = sched_pick_next(s);
    if (!nxt || nxt == cur) {                       /* 无其它就绪者：原进程继续跑，不算真切换 */
        cur->state = S_RUNNING;
        s->current = cur->pid;
        return 0;
    }
    s->ctx_switches++;
    nxt->state = S_RUNNING;
    nxt->switches++;
    s->current = nxt->pid;
    return nxt->pid;
}
