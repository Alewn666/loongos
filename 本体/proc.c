/* LoongOS 第五十七轮 —— 进程模型纯逻辑（见 proc.h 的设计取舍）
 * freestanding；只用自有循环，不依赖 libc/mmio/kmalloc，宿主可 -Werror 编译。
 */
#include "proc.h"

static void pzero(void *p, u32 n)
{
    u8 *b = (u8 *)p;
    for (u32 i = 0; i < n; i++) b[i] = 0;
}

void proc_init(struct proc *tab, int n)
{
    for (int i = 0; i < n; i++) {
        tab[i].pid = 0;
        tab[i].state = P_FREE;
        tab[i].ppid = 0;
        tab[i].exit_code = 0;
        tab[i].user_pt = 0;
        pzero(tab[i].frame, PROC_FRAME_BYTES);
    }
}

int proc_alloc(struct proc *tab, int n, u32 ppid)
{
    for (int i = 0; i < n; i++) {
        if (tab[i].state == P_FREE) {
            u32 pid = (u32)(i + 2);          /* pid 1 永久留给初始进程 */
            tab[i].pid = pid;
            tab[i].state = P_RUN;
            tab[i].ppid = ppid;
            tab[i].exit_code = 0;
            tab[i].user_pt = 0;
            return (int)pid;
        }
    }
    return -1;
}

struct proc *proc_find(struct proc *tab, int n, u32 pid)
{
    for (int i = 0; i < n; i++)
        if (tab[i].state != P_FREE && tab[i].pid == pid)
            return &tab[i];
    return 0;
}

int proc_free(struct proc *p)
{
    if (!p || p->state == P_FREE) return -1;
    p->state = P_FREE;
    p->pid = 0;
    p->ppid = 0;
    p->exit_code = 0;
    p->user_pt = 0;
    return 0;
}

int proc_zombie(struct proc *tab, int n, u32 pid, int code)
{
    struct proc *p = proc_find(tab, n, pid);
    if (!p || p->state != P_RUN) return -1;
    p->state = P_ZOMBIE;
    p->exit_code = code;
    return 0;
}

int proc_zombie_count(const struct proc *tab, int n, u32 ppid)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if (tab[i].state == P_ZOMBIE && tab[i].ppid == ppid) c++;
    return c;
}

int proc_reap(struct proc *tab, int n, u32 ppid, int *code, u32 *child_pid)
{
    for (int i = 0; i < n; i++) {
        if (tab[i].state == P_ZOMBIE && tab[i].ppid == ppid) {
            if (code) *code = tab[i].exit_code;
            if (child_pid) *child_pid = tab[i].pid;
            proc_free(&tab[i]);
            return 0;
        }
    }
    return -1;
}

int proc_copy_userspace(u64 base, u32 bytes, u32 chunk,
                        proc_mem_io rd, void *rctx,
                        proc_mem_io wr, void *wctx)
{
    if (!rd || !wr || chunk == 0) return -1;
    u8 buf[256];
    if (chunk > sizeof buf) chunk = sizeof buf;
    for (u32 off = 0; off < bytes; ) {
        u32 n = bytes - off;
        if (n > chunk) n = chunk;
        if (rd(base + off, buf, n, rctx) < 0) return -1;
        if (wr(base + off, buf, n, wctx) < 0) return -1;
        off += n;
    }
    return 0;
}
