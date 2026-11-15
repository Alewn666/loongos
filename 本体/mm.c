/* LoongOS 第五十四轮 —— 动态内存地基实现（演进路线 2.1）
 * 纯 C、零 libc、零 BSS 数据段占用（只留一个 8 字节控制页指针）。
 * 详细设计取舍见 mm.h 文件头。
 */
#include "mm.h"

#define MM_CTRL_MAGIC   0x4D4D3534u   /* "MM54" */
#define MM_BLK_ALIVE    0x4C4F4F41u   /* "LOOA" */
#define MM_BLK_FREE     0x4C4F4F46u   /* "LOOF" */
#define MM_BLK_LARGE    0x4C4F4F4Cu   /* "LOOL" */
#define MM_BLK_HDR      16u
#define MM_OWN_CTRL     0xFFu
#define MM_OWN_LARGE0   0xFEu
#define MM_OWN_LARGEN   0xFDu
#define MM_OWN_FREE     0u

/* 8 档块大小（含 16B 头）：32/64/128/256/512/1024/2048/4096 */
static const u32 mm_class_size[MM_NCLASS] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

/* 空闲块头：next 只在 FREE 状态有效；magic 区分 ALIVE / FREE / LARGE，
 * 是重复释放与野指针释放的检出依据。 */
struct mm_blk {
    u64  next;
    u32  magic;
    u8   cls;
    u8   npages;
    u16  req;
};

/* 控制页（窗口第 0 页）。所有可变状态都在这里，BSS 只有一个指针。 */
struct mm_ctrl {
    u32  magic;
    u32  bytes;
    u32  npages;
    u32  free_pages;
    u32  used_bytes;
    u32  peak_bytes;
    u32  live_objs;
    u32  alloc_cnt;
    u32  free_cnt;
    u32  fail_cnt;
    u32  badfree_cnt;
    u64  base;
    void *free_head[MM_NCLASS];
    u8   bitmap[MM_WIN_SIZE / MM_PAGE / 8];       /* 256B：2048 页 */
    u8   owner[MM_WIN_SIZE / MM_PAGE];            /* 2048B：每页归属 */
};

static struct mm_ctrl *MM;   /* 唯一的 BSS 变量（8B） */

static void mm_zero(void *d, u32 n)
{
    u8 *p = (u8 *)d;
    while (n--) *p++ = 0;
}

static int bit_get(u32 i) { return (MM->bitmap[i >> 3] >> (i & 7)) & 1; }
static void bit_set(u32 i) { MM->bitmap[i >> 3] |= (u8)(1u << (i & 7)); }
static void bit_clr(u32 i) { MM->bitmap[i >> 3] &= (u8)~(1u << (i & 7)); }

static u32 pick_class(u32 n)
{
    for (u32 c = 0; c < MM_NCLASS; c++)
        if (n + MM_BLK_HDR <= mm_class_size[c]) return c;
    return MM_NCLASS;             /* 需要走大块连续页 */
}

/* 分配 n 个连续页，返回首页号；0 = 失败（0 号是控制页，永不被分配出去） */
static u32 page_alloc_contig(u32 n)
{
    if (n == 0 || n >= MM->npages) return 0;
    u32 run = 0;
    for (u32 i = 1; i < MM->npages; i++) {
        if (!bit_get(i)) {
            if (++run == n) {
                u32 first = i - n + 1;
                for (u32 k = 0; k < n; k++) bit_set(first + k);
                MM->free_pages -= n;
                return first;
            }
        } else run = 0;
    }
    return 0;
}

/* 给某一档新开一页，切成 4096/size 个块串进空闲链表 */
static int class_grow(u32 c)
{
    u32 p = page_alloc_contig(1);
    if (!p) { MM->fail_cnt++; return -1; }
    u32 sz = mm_class_size[c];
    u32 cnt = MM_PAGE / sz;
    u8 *page = (u8 *)MM->base + (u64)p * MM_PAGE;
    for (u32 k = 0; k < cnt; k++) {
        struct mm_blk *b = (struct mm_blk *)(page + k * sz);
        b->magic  = MM_BLK_FREE;
        b->cls    = (u8)c;
        b->npages = 0;
        b->req    = 0;
        b->next   = (u64)(unsigned long)MM->free_head[c];
        MM->free_head[c] = (void *)b;
    }
    for (u32 k = 0; k < 1; k++) MM->owner[p + k] = (u8)(c + 1);
    return 0;
}

int mm_init(void *base, u32 bytes)
{
    if (!base || bytes < MM_PAGE * 4 || (bytes & (MM_PAGE - 1))) return -1;
    if (((u64)(unsigned long)base) & (MM_PAGE - 1)) return -1;
    if ((u64)bytes / MM_PAGE > (MM_WIN_SIZE / MM_PAGE)) return -1;

    MM = (struct mm_ctrl *)base;
    mm_zero((void *)MM, MM_PAGE);          /* 只清控制页，其余页按需使用 */
    MM->magic      = MM_CTRL_MAGIC;
    MM->bytes      = bytes;
    MM->npages     = bytes / MM_PAGE;
    MM->base       = (u64)(unsigned long)base;
    MM->free_pages = MM->npages - 1;       /* 第 0 页 = 控制页 */
    bit_set(0);
    MM->owner[0] = MM_OWN_CTRL;
    return 0;
}

void *kmalloc(u32 n)
{
    if (!MM || MM->magic != MM_CTRL_MAGIC || n == 0) return 0;
    /* 块头用 u16 记请求长度（保证 16B 头 / 8B 对齐），教学内核单次分配
     * 上限 65535B；超界明确失败并计入 fail_cnt，不让统计悄悄失真。 */
    if (n > 65535u) { MM->fail_cnt++; return 0; }
    u32 c = pick_class(n);
    if (c < MM_NCLASS) {
        if (!MM->free_head[c] && class_grow(c) != 0) return 0;
        struct mm_blk *b = (struct mm_blk *)MM->free_head[c];
        MM->free_head[c] = (void *)(unsigned long)b->next;
        b->magic  = MM_BLK_ALIVE;
        b->cls    = (u8)c;
        b->npages = 0;
        b->req    = (u16)n;
        MM->alloc_cnt++;
        MM->live_objs++;
        MM->used_bytes += n;
        if (MM->used_bytes > MM->peak_bytes) MM->peak_bytes = MM->used_bytes;
        return (u8 *)b + MM_BLK_HDR;
    }
    /* 大块：连续页 + 页首页头 */
    u32 need = (n + MM_BLK_HDR + MM_PAGE - 1) / MM_PAGE;
    u32 p = page_alloc_contig(need);
    if (!p) { MM->fail_cnt++; return 0; }
    struct mm_blk *b = (struct mm_blk *)((u8 *)MM->base + (u64)p * MM_PAGE);
    b->magic  = MM_BLK_LARGE;
    b->cls    = 0xFF;
    b->npages = (u8)need;
    b->req    = (u16)(n > 65535u ? 65535u : n);
    MM->owner[p] = MM_OWN_LARGE0;
    for (u32 k = 1; k < need; k++) MM->owner[p + k] = MM_OWN_LARGEN;
    MM->alloc_cnt++;
    MM->live_objs++;
    MM->used_bytes += n;
    if (MM->used_bytes > MM->peak_bytes) MM->peak_bytes = MM->used_bytes;
    return (u8 *)b + MM_BLK_HDR;
}

void *kzalloc(u32 n)
{
    void *p = kmalloc(n);
    if (p) mm_zero(p, n);
    return p;
}

static int addr_in_heap(const void *p)
{
    u64 a = (u64)(unsigned long)p;
    return MM && a >= MM->base + MM_PAGE && a < MM->base + MM->bytes;
}

void kfree(void *p)
{
    if (!p) return;                       /* free(NULL) 合法，直接返回 */
    if (!MM || MM->magic != MM_CTRL_MAGIC) { return; }
    struct mm_blk *b = (struct mm_blk *)((u8 *)p - MM_BLK_HDR);
    /* 野指针 / 已被踩坏的块头：只记账，绝不写回堆（防越界破坏） */
    if (!addr_in_heap((void *)b) || ((u64)(unsigned long)b & 7)) {
        MM->badfree_cnt++; return;
    }
    if (b->magic == MM_BLK_LARGE) {
        u32 pg = (u32)(((u64)(unsigned long)b - MM->base) / MM_PAGE);
        u32 need = b->npages;
        if (!need || pg + need > MM->npages) { MM->badfree_cnt++; return; }
        for (u32 k = 0; k < need; k++) {
            bit_clr(pg + k);
            MM->owner[pg + k] = MM_OWN_FREE;
        }
        MM->free_pages += need;
        MM->free_cnt++;
        MM->live_objs--;
        MM->used_bytes -= b->req;
        b->magic = MM_BLK_FREE;
        return;
    }
    if (b->magic != MM_BLK_ALIVE || b->cls >= MM_NCLASS) {
        MM->badfree_cnt++; return;        /* 重复释放 / 错档 / 野指针 */
    }
    u32 c = b->cls;
    b->magic  = MM_BLK_FREE;
    b->next   = (u64)(unsigned long)MM->free_head[c];
    MM->free_head[c] = (void *)b;
    MM->free_cnt++;
    MM->live_objs--;
    MM->used_bytes -= b->req;
}

int kmalloc_audit(void)
{
    if (!MM || MM->magic != MM_CTRL_MAGIC) return 1;
    int bad = 0;
    /* ① 位图与 owner 必须一致 */
    u32 free_cnt = 0;
    for (u32 i = 0; i < MM->npages; i++) {
        int used = bit_get(i);
        if (!used) free_cnt++;
        if (i == 0) {
            if (!used || MM->owner[0] != MM_OWN_CTRL) bad++;
        } else if (!used && MM->owner[i] != MM_OWN_FREE) bad++;
        else if (used && MM->owner[i] == MM_OWN_FREE) bad++;
    }
    if (free_cnt != MM->free_pages) bad++;
    /* ② 每档空闲链表的块必须落在窗口内、magic=FREE、cls 正确 */
    for (u32 c = 0; c < MM_NCLASS; c++) {
        u32 seen = 0;
        for (void *q = MM->free_head[c]; q; q = (void *)(unsigned long)((struct mm_blk *)q)->next) {
            struct mm_blk *b = (struct mm_blk *)q;
            if (!addr_in_heap((void *)b) || ((u64)(unsigned long)b & 7)) { bad++; break; }
            if (b->magic != MM_BLK_FREE || b->cls != (u8)c) bad++;
            u32 pg = (u32)(((u64)(unsigned long)b - MM->base) / MM_PAGE);
            if (pg == 0 || MM->owner[pg] != (u8)(c + 1)) bad++;
            if (++seen > MM->npages * (MM_PAGE / 32)) { bad++; break; }  /* 环路保护 */
        }
    }
    return bad;
}

/* 一轮确定性压力：显式覆盖全部 8 档边界 + 大块，再随机混合。
 * 返回本轮发现的问题数。alloc/free 全部配对，结束时活跃对象必须归零。 */
static int mm_pressure(u32 tag)
{
    enum { N = 1000 };
    void *ptr[N];
    u32   req[N];
    int   bad = 0;
    u32   a0 = MM->alloc_cnt, f0 = MM->free_cnt;
    u32   b0 = MM->badfree_cnt, e0 = MM->fail_cnt;
    /* ⚠️ 种子刻意固定、不带 tag：kmalloc_test 的两轮必须用**完全相同的
       尺寸序列**，否则峰值页数不同，「页复用」判据会误报。tag 只用于写
       模式字节，防止两轮数据全等时掩盖越界写。 */
    u32   seed = 0x12345678u;

    /* 前 8 个请求精确落在每档的最大可用值（size-16），保证 8 档都被 grow 过 */
    static const u32 edge[MM_NCLASS + 2] = {
        16, 48, 112, 240, 496, 1008, 2032, 4080,   /* 8 档 */
        4081, 6000                                   /* 大块：1 页 / 2 页 */
    };
    for (u32 i = 0; i < N; i++) {
        u32 n;
        if (i < MM_NCLASS + 2) n = edge[i];
        else { seed = seed * 1664525u + 1013904223u; n = 1 + (seed % 6000u); }
        req[i] = n;
        ptr[i] = kmalloc(n);
        if (!ptr[i]) { bad++; continue; }
        volatile u8 *q = (volatile u8 *)ptr[i];
        for (u32 k = 0; k < n; k++) q[k] = (u8)(tag + i * 7u + k * 3u + 1u);
    }
    /* 模式校验：任何相邻块越界写都会在这里露出来 */
    for (u32 i = 0; i < N; i++) {
        if (!ptr[i]) continue;
        volatile u8 *q = (volatile u8 *)ptr[i];
        for (u32 k = 0; k < req[i]; k++)
            if (q[k] != (u8)(tag + i * 7u + k * 3u + 1u)) { bad++; break; }
    }
    for (u32 i = 0; i < N; i++) kfree(ptr[i]);

    if (MM->used_bytes != 0 || MM->live_objs != 0) bad++;
    if (MM->alloc_cnt - a0 != MM->free_cnt - f0) bad++;
    if (MM->alloc_cnt - a0 != N || MM->free_cnt - f0 != N) bad++;
    if (MM->badfree_cnt != b0 || MM->fail_cnt != e0) bad++;
    return bad;
}

int kmalloc_test(void)
{
    if (!MM || MM->magic != MM_CTRL_MAGIC) return 1;
    int bad = 0;
    /* 调用前必须是空堆，否则「回到基线」的判据无意义 */
    if (MM->used_bytes != 0 || MM->live_objs != 0) bad++;

    /* ① 第一轮：预热所有档 + 1000 次模式写校验 + 零泄漏 */
    bad += mm_pressure(0x11u);
    u32 pages_after_first = MM->free_pages;

    /* ② 第二轮完全相同的序列：free_pages 不得继续下降。
     *    slab 页已在第一轮常驻、大块页每轮归还 → 相等证明页面确实被复用，
     *    而不是每轮偷偷吃掉新页（这是「零泄漏」在页粒度上的判据）。 */
    bad += mm_pressure(0x22u);
    if (MM->free_pages != pages_after_first) bad++;

    /* ③ 负例：重复释放 + 窗口外野指针必须被记下，且不破坏堆 */
    void *one = kmalloc(64);
    if (!one) bad++;
    else {
        kfree(one);
        u32 b1 = MM->badfree_cnt;
        kfree(one);                                          /* 重复释放 */
        kfree((void *)(MM->base + MM->bytes + 0x1000u));    /* 窗口外 */
        if (MM->badfree_cnt - b1 != 2) bad++;
    }
    /* ④ 峰值必须被记录过（证明水位统计真的在跑，不是恒 0） */
    if (MM->peak_bytes == 0) bad++;
    /* ⑤ 最终堆完整性 */
    if (kmalloc_audit() != 0) bad++;
    return bad;
}

void mm_stat(u32 *total_pages, u32 *free_pages, u32 *used_bytes,
             u32 *peak_bytes, u32 *live_objs, u32 *alloc_cnt,
             u32 *free_cnt, u32 *fail_cnt, u32 *badfree_cnt)
{
    if (!MM || MM->magic != MM_CTRL_MAGIC) {
        if (total_pages) *total_pages = 0;
        if (free_pages) *free_pages = 0;
        if (used_bytes) *used_bytes = 0;
        if (peak_bytes) *peak_bytes = 0;
        if (live_objs) *live_objs = 0;
        if (alloc_cnt) *alloc_cnt = 0;
        if (free_cnt) *free_cnt = 0;
        if (fail_cnt) *fail_cnt = 0;
        if (badfree_cnt) *badfree_cnt = 0;
        return;
    }
    if (total_pages) *total_pages = MM->npages;
    if (free_pages)  *free_pages  = MM->free_pages;
    if (used_bytes)  *used_bytes  = MM->used_bytes;
    if (peak_bytes)  *peak_bytes  = MM->peak_bytes;
    if (live_objs)   *live_objs   = MM->live_objs;
    if (alloc_cnt)   *alloc_cnt   = MM->alloc_cnt;
    if (free_cnt)    *free_cnt    = MM->free_cnt;
    if (fail_cnt)    *fail_cnt    = MM->fail_cnt;
    if (badfree_cnt) *badfree_cnt = MM->badfree_cnt;
}

u64 mm_heap_base(void) { return MM ? MM->base : 0; }
u32 mm_heap_bytes(void) { return MM ? MM->bytes : 0; }
u32 mm_used_bytes(void) { return (MM && MM->magic == MM_CTRL_MAGIC) ? MM->used_bytes : 0; }
