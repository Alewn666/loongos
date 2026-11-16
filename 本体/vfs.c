/* LoongOS 第五十六轮：VFS 抽象层实现（挂载表 + devfs + procfs）
 *
 * freestanding：不依赖 libc、不依赖 malloc。字符串/数字格式化全部手写。
 * typedef 在本编译单元内定义（与 fs.c 同风格），vfs.h 不含 typedef。
 */
/* 类型必须在 include vfs.h 之前定义（vfs.h 本身不含 typedef） */
#ifndef LOONGOS_VFS_TYPES_DONE
#define LOONGOS_VFS_TYPES_DONE
typedef unsigned long      u64;   /* 与 kernel.c / fs.h 一致（aarch64 LP64） */
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
#endif

#include "vfs.h"

static int vstrcmp2n(const char *a, const char *b, u32 n);

/* ---------- 内部字符串工具（freestanding） ---------- */
static u32 vstrlen(const char *s) { u32 n = 0; while (s && s[n]) n++; return n; }
static int vstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
static void vmemcpy(void *d, const void *s, u32 n)
{
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    for (u32 i = 0; i < n; i++) dd[i] = ss[i];
}

/* 反序写无符号数，返回长度；cap 用尽则截断 */
static u32 utoa(u64 v, char *out, u32 cap)
{
    char tmp[24]; u32 n = 0;
    if (v == 0) { if (cap) out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    u32 k = 0;
    while (n && k < cap) out[k++] = tmp[--n];
    return k;
}

/* 输出缓冲：所有写都带 cap，防溢出 */
struct obuf { char *p; u32 cap; u32 len; };
static void ob_putc(struct obuf *o, char c)
{
    if (o->len + 1 < o->cap) o->p[o->len++] = c;
}
static void ob_str(struct obuf *o, const char *s)
{
    while (*s) ob_putc(o, *s++);
}
static void ob_num(struct obuf *o, u64 v)
{
    char b[24]; u32 n = utoa(v, b, sizeof b);
    for (u32 i = 0; i < n; i++) ob_putc(o, b[i]);
}
/* 右对齐到 width 列（Linux /proc/meminfo 风格） */
static void ob_num_r(struct obuf *o, u64 v, u32 width)
{
    char b[24]; u32 n = utoa(v, b, sizeof b);
    for (u32 i = n; i < width; i++) ob_putc(o, ' ');
    for (u32 i = 0; i < n; i++) ob_putc(o, b[i]);
}
static void ob_finish(struct obuf *o) { o->p[o->len < o->cap ? o->len : o->cap - 1] = 0; }

/* ---------- 挂载表 ---------- */
static struct vfsmount g_mounts[VFS_MMAX];
static const struct procfs_source *g_psrc;

void procfs_set_source(const struct procfs_source *src) { g_psrc = src; }

void vfs_init(void)
{
    for (u32 i = 0; i < VFS_MMAX; i++) g_mounts[i].used = 0;
}

int vfs_mount(const char *path, const struct vnode_ops *ops, void *priv)
{
    if (!path || path[0] != '/' || !ops) return -1;
    u32 len = vstrlen(path);
    if (len < 2 || len >= VFS_MPATH) return -1;
    for (u32 i = 0; i < VFS_MMAX; i++) {
        if (g_mounts[i].used && vstrcmp(g_mounts[i].path, path) == 0) return -1; /* 重复挂载 */
    }
    for (u32 i = 0; i < VFS_MMAX; i++) {
        if (!g_mounts[i].used) {
            vmemcpy(g_mounts[i].path, path, len + 1);
            g_mounts[i].ops = ops;
            g_mounts[i].priv = priv;
            g_mounts[i].used = 1;
            return 0;
        }
    }
    return -1; /* 表满 */
}

int vfs_umount(const char *path)
{
    if (!path) return -1;
    for (u32 i = 0; i < VFS_MMAX; i++) {
        if (g_mounts[i].used && vstrcmp(g_mounts[i].path, path) == 0) {
            g_mounts[i].used = 0;
            return 0;
        }
    }
    return -1;
}

int vfs_mounted(const char *path)
{
    if (!path) return 0;
    for (u32 i = 0; i < VFS_MMAX; i++)
        if (g_mounts[i].used && vstrcmp(g_mounts[i].path, path) == 0) return 1;
    return 0;
}

u32 vfs_mount_count(void)
{
    u32 n = 0;
    for (u32 i = 0; i < VFS_MMAX; i++) if (g_mounts[i].used) n++;
    return n;
}

/* 最长前缀匹配：path == mpath 或 path 以 mpath + '/' 开头 */
int vfs_resolve(const char *path, struct vfsmount **m, char *rel, u32 relcap)
{
    if (!path || path[0] != '/' || !m) return 0;
    struct vfsmount *best = 0;
    u32 bestlen = 0;
    for (u32 i = 0; i < VFS_MMAX; i++) {
        if (!g_mounts[i].used) continue;
        const char *mp = g_mounts[i].path;
        u32 ml = vstrlen(mp);
        if (vstrlen(path) < ml) continue;
        if (vstrcmp2n(path, mp, ml) != 0) continue;
        /* 必须整段匹配：path 结尾或下一字符是 '/' */
        if (path[ml] != 0 && path[ml] != '/') continue;
        if (ml > bestlen) { best = &g_mounts[i]; bestlen = ml; }
    }
    if (!best) return 0;
    *m = best;
    const char *r = path + bestlen;
    while (*r == '/') r++;
    u32 n = 0;
    while (*r && n + 1 < relcap) rel[n++] = *r++;
    rel[n] = 0;
    return 1;
}
/* vfs_resolve 用的定长比较（避免依赖 libc strncmp） */
int vstrcmp2n(const char *a, const char *b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if ((u8)a[i] != (u8)b[i]) return (u8)a[i] - (u8)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

int vfs_read_path(const char *path, u8 *buf, u32 cap)
{
    struct vfsmount *m; char rel[128];
    if (!vfs_resolve(path, &m, rel, sizeof rel)) return -1;
    void *vh = 0;
    if (m->ops->lookup(rel, &vh) < 0) return -1;
    return m->ops->read(vh, buf, cap);
}

int vfs_stat_path(const char *path, struct vstat *st)
{
    struct vfsmount *m; char rel[128];
    if (!vfs_resolve(path, &m, rel, sizeof rel)) return -1;
    void *vh = 0;
    if (m->ops->lookup(rel, &vh) < 0) return -1;
    return m->ops->stat(vh, st);
}

int vfs_list_path(const char *path,
                  void (*cb)(const char *, u16, u32, void *), void *arg)
{
    struct vfsmount *m; char rel[128];
    if (!vfs_resolve(path, &m, rel, sizeof rel)) return -1;
    void *vh = 0;
    if (m->ops->lookup(rel, &vh) < 0) return -1;
    if (!m->ops->listdir) return -1;
    m->ops->listdir(vh, cb, arg);
    return 0;
}

/* ==================== devfs 后端 ====================
 * /dev/null  读 0 字节（写丢弃由 open 层处理，这里 read 直接 0）
 * /dev/zero  读满 cap 的 0 字节
 * /dev/console /dev/tty  读 0（输出走 console 由 shell 重定向处理）
 * /dev        根目录列出四个节点
 */
enum { DEV_NULL = 0, DEV_ZERO, DEV_CONSOLE, DEV_TTY, DEV_ROOT, DEV_N };
static const char *dev_names[DEV_N] = { "null", "zero", "console", "tty", "" };

static int dev_lookup(const char *rel, void **vh)
{
    if (!rel || !*rel) { *vh = (void *)(u64)DEV_ROOT; return 0; }
    for (int i = 0; i < DEV_ROOT; i++)
        if (vstrcmp(rel, dev_names[i]) == 0) { *vh = (void *)(u64)i; return 0; }
    return -1;
}
static int dev_read(void *vh, u8 *buf, u32 cap)
{
    u32 id = (u32)(u64)vh;
    if (id == DEV_ZERO) { for (u32 i = 0; i < cap; i++) buf[i] = 0; return (int)cap; }
    return 0; /* null / console / tty / root */
}
static void dev_listdir(void *vh, void (*cb)(const char *, u16, u32, void *), void *arg)
{
    u32 id = (u32)(u64)vh;
    if (id != DEV_ROOT) return;
    cb(".", 1, 0, arg); cb("..", 1, 0, arg);
    for (int i = 0; i < DEV_ROOT; i++) cb(dev_names[i], 2, 0, arg);
}
static int dev_stat(void *vh, struct vstat *st)
{
    u32 id = (u32)(u64)vh;
    st->type = (id == DEV_ROOT) ? 1 : 2;
    st->mode = 0; st->size = 0; st->uid = 0;
    return 0;
}
const struct vnode_ops g_devfs_ops = {
    dev_lookup, dev_read, dev_listdir, dev_stat, "devfs"
};

/* ==================== procfs 后端 ====================
 * /proc/meminfo  真实堆/BSS 数据（来自 procfs_source 回调）
 * /proc/uptime   开机秒 + 空闲秒（当前空闲恒 0，诚实标注）
 * /proc/version  内核版本串
 * /proc/cpuinfo  架构与核数
 * /proc/mounts   当前挂载表（真实遍历）
 * /proc          根目录列出以上五项
 */
enum { P_MEMINFO = 0, P_UPTIME, P_VERSION, P_CPUINFO, P_MOUNTS, P_ROOT, P_N };
static const char *proc_names[P_N] = {
    "meminfo", "uptime", "version", "cpuinfo", "mounts", ""
};

static int proc_lookup(const char *rel, void **vh)
{
    if (!rel || !*rel) { *vh = (void *)(u64)P_ROOT; return 0; }
    for (int i = 0; i < P_ROOT; i++)
        if (vstrcmp(rel, proc_names[i]) == 0) { *vh = (void *)(u64)i; return 0; }
    return -1;
}
static int proc_read(void *vh, u8 *ob, u32 cap)
{
    u32 id = (u32)(u64)vh;
    struct obuf o = { (char *)ob, cap, 0 };
    switch (id) {
    case P_MEMINFO: {
        u32 tot = g_psrc && g_psrc->mem_total_kb ? g_psrc->mem_total_kb() : 0;
        u32 fre = g_psrc && g_psrc->mem_free_kb  ? g_psrc->mem_free_kb()  : 0;
        ob_str(&o, "MemTotal:        "); ob_num_r(&o, tot, 7); ob_str(&o, " kB\n");
        ob_str(&o, "MemFree:         "); ob_num_r(&o, fre, 7); ob_str(&o, " kB\n");
        ob_str(&o, "MemAvailable:    "); ob_num_r(&o, fre, 7); ob_str(&o, " kB\n");
        ob_str(&o, "KernelStack:     "); ob_num_r(&o, 0, 7); ob_str(&o, " kB\n");
        break;
    }
    case P_UPTIME: {
        u32 up = g_psrc && g_psrc->uptime_sec ? g_psrc->uptime_sec() : 0;
        ob_num(&o, up); ob_str(&o, ".00 0.00\n");
        break;
    }
    case P_VERSION:
        ob_str(&o, g_psrc && g_psrc->version ? g_psrc->version : "LoongOS");
        ob_putc(&o, '\n');
        break;
    case P_CPUINFO:
        ob_str(&o, "Architecture:   aarch64\nProcessor:      LoongOS\n");
        ob_str(&o, "CPU(s):         ");
        ob_num(&o, g_psrc && g_psrc->ncpu ? g_psrc->ncpu() : 1);
        ob_putc(&o, '\n');
        break;
    case P_MOUNTS:
        for (u32 i = 0; i < VFS_MMAX; i++) {
            if (!g_mounts[i].used) continue;
            ob_str(&o, g_mounts[i].path); ob_putc(&o, ' ');
            ob_str(&o, g_mounts[i].path); ob_putc(&o, ' ');
            ob_str(&o, g_mounts[i].ops->name ? g_mounts[i].ops->name : "?");
            ob_str(&o, " rw 0 0\n");
        }
        break;
    default: /* 根目录 cat：空 */
        break;
    }
    ob_finish(&o);
    return (int)o.len;
}
static void proc_listdir(void *vh, void (*cb)(const char *, u16, u32, void *), void *arg)
{
    u32 id = (u32)(u64)vh;
    if (id != P_ROOT) return;
    cb(".", 1, 0, arg); cb("..", 1, 0, arg);
    for (int i = 0; i < P_ROOT; i++) cb(proc_names[i], 2, 0, arg);
}
static int proc_stat(void *vh, struct vstat *st)
{
    u32 id = (u32)(u64)vh;
    st->type = (id == P_ROOT) ? 1 : 2;
    st->mode = 0; st->uid = 0;
    st->size = (id == P_ROOT) ? 0 : (u32)vstrlen(proc_names[id]);
    return 0;
}
const struct vnode_ops g_procfs_ops = {
    proc_lookup, proc_read, proc_listdir, proc_stat, "procfs"
};
