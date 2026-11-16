/* LoongOS 第五十五轮 —— 统一 fd / pipe 对象层实现
 * 编译：内核侧由 build.sh 纳入 KERN_SRCS；宿主单测 cc ... usysfd.c mm.c
 *      （mm.c 提供 kmalloc/kfree，宿主单测先 mm_init 一块静态窗口）。
 * 本文件是纯 C / freestanding，不 include 任何内核头、不碰串口、不碰 MMIO。
 */
#include "usysfd.h"
#include "mm.h"          /* 第五十四轮动态内存地基：ring = kmalloc(UPIPE_RING) */

struct upipe {
    u8  *ring;           /* kmalloc(UPIPE_RING)；0 = 未分配 */
    u32  head, tail, len;
    int  readers, writers;
    int  used;
};
struct ofd {
    u8   kind;
    u8   pipe;           /* UPIPE_MAX 下标 */
    u16  _rsv;
};

static struct ofd   g_ofd[UFD_MAX];
static struct upipe g_pipe[UPIPE_MAX];
static ufd_console_fn g_console = 0;
static u32 g_alloc_fail = 0;

static void zzero(u8 *p, u32 n) { for (u32 i = 0; i < n; i++) p[i] = 0; }

void ufd_set_console(ufd_console_fn fn) { g_console = fn; }

/* 开机一次：fd0/1/2 = console，其余 free；pipe 对象全清（不分配 ring） */
void ufd_init(void)
{
    for (int i = 0; i < UFD_MAX; i++) {
        g_ofd[i].kind = UFD_FREE;
        g_ofd[i].pipe = 0;
        g_ofd[i]._rsv = 0;
    }
    for (int i = 0; i < UPIPE_MAX; i++) {
        g_pipe[i].ring = 0;
        g_pipe[i].head = g_pipe[i].tail = g_pipe[i].len = 0;
        g_pipe[i].readers = g_pipe[i].writers = 0;
        g_pipe[i].used = 0;
    }
    g_ofd[0].kind = g_ofd[1].kind = g_ofd[2].kind = UFD_CONSOLE;
    g_alloc_fail = 0;
}

/* 每次装载用户程序前调用：释放上一轮遗留的 ring，复位到干净 console 状态。
   ufd_console_fn 不清 —— 开机注入一次即可。 */
void ufd_reset(void)
{
    for (int i = 0; i < UPIPE_MAX; i++) {
        if (g_pipe[i].ring) { kfree(g_pipe[i].ring); g_pipe[i].ring = 0; }
    }
    ufd_init();
}

int ufd_kind(int fd)
{
    if (fd < 0 || fd >= UFD_MAX) return -1;
    return (int)g_ofd[fd].kind;
}

static int find_free_fd(void)
{
    for (int i = (int)UFD_NEWBASE; i < UFD_MAX; i++)
        if (g_ofd[i].kind == UFD_FREE) return i;
    return -1;
}
static int find_free_pipe(void)
{
    for (int i = 0; i < UPIPE_MAX; i++)
        if (!g_pipe[i].used) return i;
    return -1;
}
static int valid_obj(int fd)
{
    return fd >= 0 && fd < UFD_MAX && g_ofd[fd].kind != UFD_FREE;
}

int ufd_pipe(int fds[2])
{
    if (!fds) return -1;
    int p = find_free_pipe();
    int r = find_free_fd();
    if (p < 0 || r < 0) return -1;
    /* 写端要再找一个新 fd（不能和读端同一个） */
    g_ofd[r].kind = UFD_PIPE_R;          /* 先占住，find_free_fd 才会跳到下一个 */
    g_ofd[r].pipe = (u8)p;
    int w = find_free_fd();
    if (w < 0) { g_ofd[r].kind = UFD_FREE; return -1; }

    u8 *ring = (u8 *)kmalloc(UPIPE_RING);
    if (!ring) {
        g_ofd[r].kind = UFD_FREE;
        g_alloc_fail++;
        return -1;
    }
    zzero(ring, UPIPE_RING);
    g_pipe[p].ring = ring;
    g_pipe[p].head = g_pipe[p].tail = g_pipe[p].len = 0;
    g_pipe[p].readers = 1;
    g_pipe[p].writers = 1;
    g_pipe[p].used = 1;

    g_ofd[w].kind = UFD_PIPE_W;
    g_ofd[w].pipe = (u8)p;
    fds[0] = r;
    fds[1] = w;
    return 0;
}

int ufd_dup(int oldfd)
{
    if (!valid_obj(oldfd)) return -1;
    int n = find_free_fd();
    if (n < 0) return -1;
    g_ofd[n].kind = g_ofd[oldfd].kind;
    g_ofd[n].pipe = g_ofd[oldfd].pipe;
    int p = g_ofd[oldfd].pipe;
    if (g_ofd[oldfd].kind == UFD_PIPE_R) g_pipe[p].readers++;
    if (g_ofd[oldfd].kind == UFD_PIPE_W) g_pipe[p].writers++;
    return n;
}

int ufd_dup2(int oldfd, int newfd)
{
    if (!valid_obj(oldfd) || newfd < 0 || newfd >= UFD_MAX) return -1;
    if (oldfd == newfd) return newfd;
    (void)ufd_close(newfd);              /* 先释放 newfd 原对象（含 pipe 引用） */
    g_ofd[newfd].kind = g_ofd[oldfd].kind;
    g_ofd[newfd].pipe = g_ofd[oldfd].pipe;
    int p = g_ofd[oldfd].pipe;
    if (g_ofd[oldfd].kind == UFD_PIPE_R) g_pipe[p].readers++;
    if (g_ofd[oldfd].kind == UFD_PIPE_W) g_pipe[p].writers++;
    return newfd;
}

int ufd_close(int fd)
{
    if (!valid_obj(fd)) return -1;
    int k = g_ofd[fd].kind;
    int p = g_ofd[fd].pipe;
    if (k == UFD_PIPE_R && g_pipe[p].used) g_pipe[p].readers--;
    if (k == UFD_PIPE_W && g_pipe[p].used) g_pipe[p].writers--;
    if ((k == UFD_PIPE_R || k == UFD_PIPE_W) && g_pipe[p].used &&
        g_pipe[p].readers <= 0 && g_pipe[p].writers <= 0) {
        if (g_pipe[p].ring) { kfree(g_pipe[p].ring); g_pipe[p].ring = 0; }
        g_pipe[p].used = 0;
        g_pipe[p].len = g_pipe[p].head = g_pipe[p].tail = 0;
    }
    /* fd 0/1/2 是标准流：close 后回到 console，而不是 FREE（sh 靠这个恢复 stdout） */
    g_ofd[fd].kind = (fd < 3) ? UFD_CONSOLE : UFD_FREE;
    g_ofd[fd].pipe = 0;
    return 0;
}

long ufd_write(int fd, const u8 *src, u32 n)
{
    if (!valid_obj(fd) || (!src && n)) return -1;
    int k = g_ofd[fd].kind;
    if (k == UFD_CONSOLE) {
        if (!g_console) return -1;
        g_console(src, n);
        return (long)n;
    }
    if (k != UFD_PIPE_W) return -1;      /* 读端不可写 */
    int p = g_ofd[fd].pipe;
    struct upipe *q = &g_pipe[p];
    if (!q->used || !q->ring) return -1;
    u32 space = UPIPE_RING - q->len;
    u32 w = n < space ? n : space;
    for (u32 i = 0; i < w; i++) {
        q->ring[q->tail] = src[i];
        q->tail = (q->tail + 1) % UPIPE_RING;
    }
    q->len += w;
    return (long)w;
}

long ufd_read(int fd, u8 *dst, u32 n)
{
    if (!valid_obj(fd) || (!dst && n)) return -1;
    int k = g_ofd[fd].kind;
    if (k == UFD_CONSOLE) return 0;      /* 用户态 sh 不从 fd0 读串口，输入走 argv */
    if (k != UFD_PIPE_R) return -1;      /* 写端不可读 */
    int p = g_ofd[fd].pipe;
    struct upipe *q = &g_pipe[p];
    if (!q->used || !q->ring) return -1;
    u32 r = n < q->len ? n : q->len;
    for (u32 i = 0; i < r; i++) {
        dst[i] = q->ring[q->head];
        q->head = (q->head + 1) % UPIPE_RING;
    }
    q->len -= r;
    return (long)r;
}

int ufd_live_pipes(void)
{
    int n = 0;
    for (int i = 0; i < UPIPE_MAX; i++) if (g_pipe[i].used) n++;
    return n;
}
u32 ufd_pipe_bytes(int fd)
{
    if (!valid_obj(fd)) return 0;
    int k = g_ofd[fd].kind;
    if (k != UFD_PIPE_R && k != UFD_PIPE_W) return 0;
    return g_pipe[g_ofd[fd].pipe].len;
}
u32 ufd_alloc_fail(void) { return g_alloc_fail; }
