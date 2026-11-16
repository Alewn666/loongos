/* LoongOS 第五十六轮：VFS 抽象层（对齐演进路线 §3.1 / 第 56 轮排期）
 *
 * 设计目标：让 /proc、/dev 这类伪文件系统与真实 loongfs 共用一套
 * 「挂载点 → 操作表」抽象，新增文件系统只实现 vnode_ops、不改上层命令。
 *
 * 最小侵入原则（保 golden_behavior.txt 29 条逐字基线）：
 *   - VFS 只接管“显式挂载点前缀”的路径（默认 /proc、/dev）；
 *   - 老路径一律先走 fs_lookup/fs_readat，命中即返回，VFS 完全不参与；
 *   - 只有 fs_lookup 失败且路径命中某个挂载点前缀，才交给该挂载点的 ops。
 *   因此 cat /etc/hostname、ls /nope 等既有行为逐字不变。
 *
 * 本头 freestanding，u8/u16/u32/u64 由调用方（loongos.h / 宿主单测）先定义。
 * 不引入 malloc：挂载表固定 VFS_MMAX 项（静态数组），伪文件内容现场生成。
 */
#ifndef LOONGOS_VFS_H
#define LOONGOS_VFS_H

#define VFS_MMAX       8u      /* 同时挂载点上界 */
#define VFS_MPATH      32u     /* 挂载点路径最大长度（含结尾） */
#define VFS_NAME       48u     /* vnode 名字最大长度 */

/* 一个 vnode 的元数据（用户可见形态，避免暴露各后端内部结构） */
struct vstat {
    u16 type;                  /* FS_T_DIR / FS_T_FILE */
    u16 mode;
    u32 size;                  /* 字节数；未知动态文件可给 0 */
    u32 uid;
};

/* 文件系统操作表：新增文件系统实现这组回调即可挂载 */
struct vnode_ops {
    /* 把挂载点内的相对路径解析成 vnode handle；返回 0=命中，<0=未找到 */
    int  (*lookup)(const char *rel, void **vh);
    /* 读 vnode 内容到 buf（最多 cap 字节），返回实际字节数；<0=错误 */
    int  (*read)(void *vh, u8 *buf, u32 cap);
    /* 列目录：对每个条目回调 cb(name, type, size, arg) */
    void (*listdir)(void *vh, void (*cb)(const char *, u16, u32, void *), void *arg);
    /* 填充 vstat；返回 0 成功 */
    int  (*stat)(void *vh, struct vstat *st);
    /* 名字（用于 /proc/mounts、调试） */
    const char *name;
};

struct vfsmount {
    int used;                  /* 0 = 空槽 */
    char path[VFS_MPATH];      /* 挂载点绝对路径，如 "/proc" */
    const struct vnode_ops *ops;
    void *priv;                /* 后端私有数据（可为 0） */
};

/* 挂载表管理 */
void  vfs_init(void);
int   vfs_mount(const char *path, const struct vnode_ops *ops, void *priv);
int   vfs_umount(const char *path);
int   vfs_mounted(const char *path);
u32   vfs_mount_count(void);

/* 路径解析：命中挂载点则填 *m 与 *rel，返回 1；未命中返回 0 */
int   vfs_resolve(const char *path, struct vfsmount **m, char *rel, u32 relcap);

/* 统一读接口：命中挂载点 vnode 则读，返回字节数；未命中返回 -1（调用方走老路径） */
int   vfs_read_path(const char *path, u8 *buf, u32 cap);
/* 统一 stat */
int   vfs_stat_path(const char *path, struct vstat *st);
/* 列挂载点目录 */
int   vfs_list_path(const char *path,
                    void (*cb)(const char *, u16, u32, void *), void *arg);

/* ---- 后端：devfs 与 procfs ---- */
extern const struct vnode_ops g_devfs_ops;
extern const struct vnode_ops g_procfs_ops;

/* procfs 需要的数据源由内核注入（避免 vfs.c 直接依赖 kernel.c 全局） */
struct procfs_source {
    u32 (*mem_total_kb)(void);
    u32 (*mem_free_kb)(void);
    u32 (*uptime_sec)(void);
    u64 (*wall_epoch)(void);
    const char *version;
    u32 (*ncpu)(void);
};
void  procfs_set_source(const struct procfs_source *src);

#endif /* LOONGOS_VFS_H */
