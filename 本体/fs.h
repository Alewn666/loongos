/* LoongOS 阶段4 - 极简文件系统（内存盘版）接口
 * 磁盘布局（每块 4KB，共 256 块 = 1MB）：
 *   block 0        superblock
 *   block 1        空闲位图（256 bit）
 *   block 2..5     inode 表（128 个 inode × 128B = 4 块）
 *   block 6..255   数据块
 * 以后换 virtio-blk 只需要改 bread/bwrite 两层，上面的逻辑不动。 */

#ifndef LOONGOS_FS_H
#define LOONGOS_FS_H

typedef unsigned long u64;
typedef unsigned int  u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define FS_MAGIC     0x4C4F4F53u   /* "LOOS" = LoongOS（灵珑系统） */
#define FS_BLOCK     4096u
#define FS_BLOCKS    256u          /* 1MB 内存盘 */
#define FS_NINODE    128u
#define FS_ROOT      1             /* 根目录固定是 1 号 inode（0 号恒空 = "无此项"） */

#define FS_B_SUPER   0u
#define FS_B_BITMAP  1u
#define FS_B_ITABLE  2u
#define FS_N_ITBL    4u
#define FS_B_DATA    (FS_B_ITABLE + FS_N_ITBL)   /* 6：数据区起点 */

#define FS_T_NONE    0
#define FS_T_DIR     1
#define FS_T_FILE    2

/* —— 阶段5 权限 —— */
#define FS_OK        0
#define FS_NOENT    (-1)
#define FS_DENIED   (-2)     /* 权限不够：不是你的文件，且你不是 root */
#define FM_PRIVATE   0x01    /* mode 位：私密（只有属主/root 能读，/etc/shadow 用） */

struct super {
    u32 magic, block_size, total_blocks, ninode, root, pad;
};
struct inode {
    u16 type;                 /* FS_T_* */
    u16 nlink;                /* 被几处引用（目录数） */
    u32 size;                 /* 字节数 */
    u64 mtime;                /* 秒（用 tick/10） */
    u64 direct[10];           /* 10 个直接块 */
    u64 indirect;             /* 一个间接块（512 个块号）→ 最大文件 2MB+40KB */
    u64 _pad[2];              /* 16B 补齐（u64 需 8 对齐，必须紧跟 indirect） */
    u16 uid;                  /* 属主 uid（阶段5）：0 = root */
    u8  mode;                 /* FM_* 位（阶段5） */
    u8  _rsv;                 /* 保留 */
    u32 _rsv2;                /* 恰好 128B：104+16+2+1+1+4 */
};
struct dirent {
    u16 inum;                 /* 0 = 空槽 */
    char name[62];            /* 字节名（UTF-8 也行），NUL 结尾 */
};

/* —— 公共 API（kernel.c 的 shell 用这些） —— */
void fs_init(void);                                  /* 内存盘：格式化 + 演示数据 */
int  fs_format_disk(u64 part_lba, u32 nblocks);      /* 在磁盘分区上格式化（阶段6③） */
int  fs_mount_disk(u64 part_lba, u32 nblocks);       /* 挂载磁盘上已有 FS（0 = 成功） */
int  fs_is_disk(void);                               /* 当前是否跑在磁盘上 */
u32  fs_blocks(void);                                /* 当前块数 */
int  fs_last_io_err(void);                           /* 最近一次块 I/O 是否出错 */
int  fs_lookup(const char *path);                   /* 绝对路径 → inode 号；找不到 -1 */
int  fs_isdir(int ino);
void fs_list(int ino, void (*cb)(const struct dirent *, int ino, void *), void *arg);
int  fs_readfile(int ino, u8 *buf, u32 cap);        /* 读整个文件，返回字节数；-1 失败 */
int  fs_createfile(const char *path);               /* 建普通文件；已存在/父目录缺 → -1 */
int  fs_mkdir(const char *path);                    /* 建目录 */
int  fs_setfile(int ino, const u8 *buf, u32 len);   /* 截断重写；0 成功 */
int  fs_unlink(const char *path);                   /* 删文件或空目录；0 成功 */
void fs_statfs(u32 *used, u32 *total);              /* 位图统计 */
int  fs_filetype(int ino);                          /* FS_T_*（查不到 -1） */
int  fs_filesize(int ino);                          /* 字节数（查不到 -1） */

/* —— 阶段5：身份与权限 ——
 * 规则（教学版 DAC）：
 *   写/删：属主或 root；建文件：父目录属主或 root；
 *   读：谁都能读，除非 inode 带 FM_PRIVATE（那时只有属主和 root）。 */
void fs_setuid(int uid);                            /* 设「当前操作者」（su 后由 shell 调） */
int  fs_getuid(void);
int  fs_chown(int ino, int uid);                    /* 改属主（0 成功） */
int  fs_chmod(int ino, int mode);                   /* 改 mode（0 成功） */
int  fs_inode_uid(int ino);                         /* 查属主（查不到 -1） */

/* fs.c 顺手提供的小工具（无 libc） */
u32  fs_strlen(const char *s);
int  fs_strcmp(const char *a, const char *b);
void fs_memcpy(void *d, const void *s, u32 n);
void fs_memset(void *d, int v, u32 n);

#endif
