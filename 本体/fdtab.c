/* LoongOS 58.0-A —— 每进程 FD 表纯逻辑（见 fdtab.h 契约）
 * freestanding；只用自有循环，不依赖 libc / mmio / kmalloc / 串口。
 */
#include "fdtab.h"

static void fzero(void *p, u32 n)
{
    u8 *b = (u8 *)p;
    for (u32 i = 0; i < n; i++) b[i] = 0;
}

void fdt_pool_init(struct ofd *pool, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        pool[i].ino = 0;
        pool[i].off = 0;
        pool[i].len = 0;
        pool[i].ref = 0;
        pool[i].kind = FDT_KIND_FREE;
        pool[i].used = 0;
    }
}

static int pool_alloc(struct ofd *pool, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (!pool[i].used) return (int)i;
    return -1;
}

int fdt_ofd_new_file(struct ofd *pool, u32 n, u32 ino, u32 len)
{
    int i = pool_alloc(pool, n);
    if (i < 0) return -1;
    pool[i].ino = ino;
    pool[i].off = 0;
    pool[i].len = len;
    pool[i].ref = 0;              /* install 时 ref++ */
    pool[i].kind = FDT_KIND_FILE;
    pool[i].used = 1;
    return i;
}

int fdt_ofd_new_console(struct ofd *pool, u32 n)
{
    int i = pool_alloc(pool, n);
    if (i < 0) return -1;
    pool[i].ino = 0;
    pool[i].off = 0;
    pool[i].len = 0;
    pool[i].ref = 0;
    pool[i].kind = FDT_KIND_CONS;
    pool[i].used = 1;
    return i;
}

int fdt_ofd_ref(struct ofd *o)
{
    if (!o || !o->used) return -1;
    if (o->ref < 0xFFFF) o->ref++;
    return (int)o->ref;
}

int fdt_ofd_unref(struct ofd *o)
{
    if (!o || !o->used) return 0;
    if (o->ref > 0) o->ref--;
    if (o->ref == 0) {
        o->used = 0;
        o->ino = o->off = o->len = 0;
        o->kind = FDT_KIND_FREE;
        return 1;                 /* 已释放 */
    }
    return 0;                     /* 仍被引用 */
}

static int fd_free_slot(const struct fd_table *t)
{
    for (u32 i = 0; i < FDT_NFD; i++)
        if (t->slot[i] == 0) return (int)i;
    return -1;
}

int fdt_get(struct fd_table *t, u32 fd)
{
    if (fd >= FDT_NFD || t->slot[fd] == 0) return -1;
    return (int)t->slot[fd] - 1;   /* 解开 +1 编码 */
}

int fdt_init(struct fd_table *t, struct ofd *pool, u32 n)
{
    fzero(t->slot, sizeof t->slot);
    /* fd 0/1/2 各指向独立 console ofd —— 与老 g_fd 的 0/1/2 语义对齐。 */
    for (u32 fd = 0; fd < 3; fd++) {
        int idx = fdt_ofd_new_console(pool, n);
        if (idx < 0) return -1;
        t->slot[fd] = (u8)(idx + 1);
        fdt_ofd_ref(&pool[idx]);
    }
    return 0;
}

void fdt_destroy(struct fd_table *t, struct ofd *pool, u32 n)
{
    for (u32 fd = 0; fd < FDT_NFD; fd++) {
        if (t->slot[fd]) {
            u32 idx = (u32)t->slot[fd] - 1;
            if (idx < n) fdt_ofd_unref(&pool[idx]);
            t->slot[fd] = 0;
        }
    }
}

int fdt_install(struct fd_table *t, struct ofd *pool, u32 n, u32 ofd_idx)
{
    if (ofd_idx >= n || !pool[ofd_idx].used) return -1;
    int fd = fd_free_slot(t);
    if (fd < 0) return -1;
    t->slot[fd] = (u8)(ofd_idx + 1);
    fdt_ofd_ref(&pool[ofd_idx]);
    return fd;
}

int fdt_close(struct fd_table *t, struct ofd *pool, u32 n, u32 fd)
{
    if (fd >= FDT_NFD || t->slot[fd] == 0) return -1;
    u32 idx = (u32)t->slot[fd] - 1;
    t->slot[fd] = 0;
    if (idx < n) fdt_ofd_unref(&pool[idx]);
    return 0;
}

int fdt_dup(struct fd_table *t, struct ofd *pool, u32 n, u32 src)
{
    int idx = fdt_get(t, src);
    if (idx < 0) return -1;
    return fdt_install(t, pool, n, (u32)idx);
}

int fdt_dup2(struct fd_table *t, struct ofd *pool, u32 n, u32 src, u32 dst)
{
    int idx = fdt_get(t, src);
    if (idx < 0 || dst >= FDT_NFD) return -1;
    if (src == dst) return (int)dst;
    if (t->slot[dst]) {                 /* 先关掉 dst 原对象（含 ref--） */
        u32 old = (u32)t->slot[dst] - 1;
        t->slot[dst] = 0;
        if (old < n) fdt_ofd_unref(&pool[old]);
    }
    t->slot[dst] = (u8)(idx + 1);
    fdt_ofd_ref(&pool[idx]);
    return (int)dst;
}

int fdt_fork(const struct fd_table *parent, struct fd_table *child,
             struct ofd *pool, u32 n)
{
    int copied = 0;
    for (u32 fd = 0; fd < FDT_NFD; fd++) {
        if (parent->slot[fd]) {
            u32 idx = (u32)parent->slot[fd] - 1;
            if (idx >= n || !pool[idx].used) return -1;   /* 父表引用了坏 ofd */
            child->slot[fd] = parent->slot[fd];           /* 同一 ofd */
            fdt_ofd_ref(&pool[idx]);                      /* 共享 -> ref++ */
            copied++;
        } else {
            child->slot[fd] = 0;
        }
    }
    return copied;
}

int fdt_pool_live(const struct ofd *pool, u32 n)
{
    int c = 0;
    for (u32 i = 0; i < n; i++) if (pool[i].used) c++;
    return c;
}

u32 fdt_pool_refsum(const struct ofd *pool, u32 n)
{
    u32 s = 0;
    for (u32 i = 0; i < n; i++) if (pool[i].used) s += pool[i].ref;
    return s;
}
