/* LoongOS 阶段4 - 极简文件系统（内存盘）实现
 * 经典三层：superblock + inode + 目录项；块分配走位图；文件块经 bmap（直接10 + 间接1）。
 * 无 libc：自带 memcpy/strlen/strcmp；无动态内存：一切都在 1MB 的 fs_disk 数组里。 */

#include "fs.h"

extern volatile u64 tick;    /* kernel.c 的时钟，mtime 用 tick/10 折算秒 */

/* ---------------- 内存盘本体（以后换 virtio-blk 只动 bread/bwrite） ---------------- */
static u8 fs_disk[FS_BLOCKS * FS_BLOCK];

/* ⚠️ 全 fs 只用这一块 4KB scratch，绝不放栈上——阶段4 第一版把 blk[4096] 放栈上，
   cat→fread_bytes→bmap 三层嵌套直接打爆 16KB 内核栈（表现为各种诡异路径错误）。
   嵌套审计：每次 bread(scratch) 后立刻把需要的数据拷出到局部/inode，
   任何函数不得跨"另一次 scratch 使用"持有其中数据。
   前提 = fs 只被 shell 一个任务串行调用（worker 不碰 fs）；以后 worker 要用必须先加锁。 */
static u8 fs_scratch[FS_BLOCK] __attribute__((aligned(8)));
static const u8 fs_zero[FS_BLOCK] = { 0 };   /* 零块模板（.rodata），balloc 给新块清零用 */

/* ---- 阶段6③：磁盘后端 ----
   装上磁盘后 bread/bwrite 直接走 virtio-blk（每块 4KB = 8 个 512B 扇区，一次请求）。
   内存盘模式（没装盘 / 从 ISO 跑）保持原样。 */
extern int vb_read_block(u64 blk_no, u64 part_lba, void *buf);
extern int vb_write_block(u64 blk_no, u64 part_lba, const void *buf);

static int  fs_on_disk = 0;        /* 1 = 根文件系统在磁盘分区上 */
static u64  fs_part_lba = 0;       /* 分区起始 LBA（512B 扇区） */
static u32  fs_nblocks = FS_BLOCKS;/* 运行期块数（内存盘 256 = 1MB；磁盘最多 32768 = 128MB，
                                      因为位图只有一块 4KB = 32768 bit） */
static int  fs_io_err = 0;         /* 最近一次块 I/O 是否出错（诊断用） */

static void bread(u32 b, void *buf)
{
    if (fs_on_disk) {
        if (vb_read_block(b, fs_part_lba, buf) != 0) { fs_io_err = 1; fs_memset(buf, 0, FS_BLOCK); }
        return;
    }
    fs_memcpy(buf, fs_disk + (u32)b * FS_BLOCK, FS_BLOCK);
}
static void bwrite(u32 b, const void *buf)
{
    if (fs_on_disk) {
        if (vb_write_block(b, fs_part_lba, buf) != 0) fs_io_err = 1;
        return;
    }
    fs_memcpy(fs_disk + (u32)b * FS_BLOCK, buf, FS_BLOCK);
}

/* ---------------- 当前操作者（阶段5 DAC） ---------------- */
static int fs_uid = 0;      /* 0 = root；su 切换用户时 shell 调 fs_setuid 同步 */
void fs_setuid(int uid) { fs_uid = uid; }
int  fs_getuid(void) { return fs_uid; }
/* 能不能改/删这个 inode：root 通吃，否则必须是属主 */
static int can_mutate(const struct inode *in) { return fs_uid == 0 || in->uid == fs_uid; }

/* ---------------- 无 libc 小工具 ---------------- */
u32 fs_strlen(const char *s) { u32 n = 0; while (*s++) n++; return n; }
int fs_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
void fs_memcpy(void *d, const void *s, u32 n)
{
    u64 *xd = d; const u64 *xs = s;
    while (n >= 8) { *xd++ = *xs++; n -= 8; }
    u8 *pd = (u8 *)xd; const u8 *ps = (const u8 *)xs;
    while (n--) *pd++ = *ps++;
}
/* 零/填充：8 字节一次写（原来逐字节，1MB 格式化时要跑 100 万次）
   ⚠️ 指针可能不是 8 字节对齐（目录项名之类），所以先对齐再走快路 */
void fs_memset(void *d, int v, u32 n)
{
    u8 *p = d;
    u8 b = (u8)v;
    u64 pat = 0x0101010101010101ULL * (u64)b;
    while (n && ((u64)p & 7)) { *p++ = b; n--; }     /* 头部对齐 */
    u64 *q = (u64 *)p;
    while (n >= 8) { *q++ = pat; n -= 8; }           /* 主体：8 字节 */
    p = (u8 *)q;
    while (n--) *p++ = b;                            /* 尾巴 */
}
static int name_ok(const char *s, u32 max)   /* 非空、不超长、无 '/' */
{
    u32 n = fs_strlen(s);
    return n > 0 && n <= max;
}

/* ---------------- 块位图 ---------------- */
static u8 bitmap[FS_BLOCK];                   /* 缓存整块位图，改完写回 */

static void bmp_load(void) { bread(FS_B_BITMAP, bitmap); }
static void bmp_save(void) { bwrite(FS_B_BITMAP, bitmap); }
static int  bmp_get(u32 b)  { return (bitmap[b / 8] >> (b % 8)) & 1; }
static void bmp_set(u32 b)  { bitmap[b / 8] |= (u8)(1u << (b % 8)); }
static void bmp_clr(u32 b)  { bitmap[b / 8] &= (u8)~(1u << (b % 8)); }

static int balloc(void)
{
    bmp_load();
    for (u32 b = FS_B_DATA; b < fs_nblocks; b++) {
        if (!bmp_get(b)) {
            bmp_set(b);
            bmp_save();
            bwrite(b, fs_zero);               /* 新块先清零（目录/间接块都靠它） */
            return (int)b;
        }
    }
    return -1;                                /* 盘满 */
}
static void bfree(u32 b)
{
    if (b < FS_B_DATA || b >= fs_nblocks) return;
    bmp_load();
    bmp_clr(b);
    bmp_save();
}

/* ---------------- inode 读写 ---------------- */
static void iget(int ino, struct inode *out)
{
    u8 *blk = fs_scratch;
    bread(FS_B_ITABLE + (u32)ino / (FS_BLOCK / sizeof(struct inode)), blk);
    /* ⚠️ 必须 memcpy 拷出，绝不能 `*out = *(struct inode *)(blk+off)`：
       blk 是刚经 u64 写入的字节缓冲，-O2 strict-aliasing 下 GCC 认为它不可能
       被按 struct inode 读，会把读操作优化掉/重排——宿主机 -O0 测不出来，
       内核 -O2 直接表现为"inode 全是垃圾"（阶段4 踩坑实录）。 */
    fs_memcpy(out, blk + (u32)ino % (FS_BLOCK / sizeof(struct inode)) * sizeof(struct inode),
              sizeof(struct inode));
}
static void iput(int ino, const struct inode *in)
{
    u8 *blk = fs_scratch;
    u32 per = FS_BLOCK / sizeof(struct inode);
    bread(FS_B_ITABLE + (u32)ino / per, blk);
    fs_memcpy(blk + (u32)ino % per * sizeof(struct inode), in, sizeof(struct inode));
    bwrite(FS_B_ITABLE + (u32)ino / per, blk);
}
static int ialloc(u16 type, u16 uid)
{
    for (u32 i = 1; i < FS_NINODE; i++) {
        struct inode in;
        iget((int)i, &in);
        if (in.type == FS_T_NONE) {
            fs_memset(&in, 0, sizeof(in));
            in.type = type;
            in.nlink = 1;
            in.uid = uid;                   /* 属主（阶段5） */
            in.mtime = tick / 10;
            iput((int)i, &in);
            return (int)i;
        }
    }
    return -1;                                /* inode 用尽 */
}

/* ---------------- 文件块映射：直接 10 块 + 间接 1 块 ---------------- */
static u32 bmap(struct inode *in, u32 fblk, int alloc)   /* 文件逻辑块号 → 盘块号；0=没映射 */
{
    if (fblk < 10) {
        if (in->direct[fblk] == 0 && alloc) {
            int b = balloc();
            if (b < 0) return 0;
            in->direct[fblk] = (u32)b;
        }
        return in->direct[fblk];
    }
    fblk -= 10;
    if (fblk >= 512) return 0;                /* 超大文件不伺候 */
    /* 间接块表走 scratch：iblk 指针指向 scratch（u32 视图，8 字节对齐已保证） */
    if (in->indirect == 0) {
        if (!alloc) return 0;
        int b = balloc();
        if (b < 0) return 0;
        in->indirect = (u32)b;
    }
    bread(in->indirect, fs_scratch);
    u32 *iblk = (u32 *)fs_scratch;
    u32 ent = iblk[fblk];
    if (ent == 0 && alloc) {
        int b = balloc();                     /* balloc 不碰 scratch（用 fs_zero），安全 */
        if (b < 0) return 0;
        iblk[fblk] = (u32)b;
        bwrite(in->indirect, fs_scratch);
        ent = (u32)b;
    }
    return ent;
}

/* 按字节区间读文件内容（自动跨块），返回实读字节 */
static int fread_bytes(struct inode *in, u32 off, void *buf, u32 len)
{
    if (off >= in->size) return 0;
    if (off + len > in->size) len = in->size - off;
    u8 *blk = fs_scratch;
    u32 done = 0;
    while (done < len) {
        u32 pos = off + done;
        u32 b = bmap(in, pos / FS_BLOCK, 0);
        if (!b) break;
        bread(b, blk);
        u32 chunk = FS_BLOCK - pos % FS_BLOCK;
        if (chunk > len - done) chunk = len - done;
        fs_memcpy((u8 *)buf + done, blk + pos % FS_BLOCK, chunk);
        done += chunk;
    }
    return (int)done;
}

/* ---------------- 目录操作 ---------------- */
/* 在目录里找名字 → inode 号；*slot = 目录项下标（-1=没找到） */
static int dirlookup(int dino, const char *name, int *slot)
{
    struct inode di;
    iget(dino, &di);
    if (di.type != FS_T_DIR) return -1;
    struct dirent de;
    for (u32 i = 0; (i * sizeof(de)) < di.size; i++) {
        if (fread_bytes(&di, i * sizeof(de), &de, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum != 0 && fs_strcmp(de.name, name) == 0) {
            if (slot) *slot = (int)i;
            return de.inum;
        }
    }
    return -1;
}

/* 往目录里塞一项（有空槽用空槽，目录长满就追加一块） */
static int dir_add(int dino, const char *name, int ino)
{
    if (!name_ok(name, sizeof(((struct dirent *)0)->name) - 1)) return -1;
    struct inode di;
    iget(dino, &di);
    if (di.type != FS_T_DIR) return -1;
    if (dirlookup(dino, name, 0) >= 0) return -1;    /* 已存在 */
    struct dirent de;
    for (u32 i = 0; (i * sizeof(de)) < di.size; i++) {
        if (fread_bytes(&di, i * sizeof(de), &de, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum == 0) {                          /* 复用空槽 */
            de.inum = (u16)ino;
            fs_memset(de.name, 0, sizeof(de.name));
            fs_memcpy(de.name, name, fs_strlen(name));
            struct inode tmp = di;                   /* 写这块之前 bmap 可能给 di 补块 */
            u32 b = bmap(&tmp, i * sizeof(de) / FS_BLOCK, 1);   /* bmap 用过 scratch 之后才轮到写块 */
            if (!b) return -1;
            di = tmp;
            u8 *blk = fs_scratch;
            bread(b, blk);
            fs_memcpy(blk + (i * sizeof(de)) % FS_BLOCK, &de, sizeof(de));
            bwrite(b, blk);
            di.mtime = tick / 10;
            iput(dino, &di);
            return 0;
        }
    }
    /* 没空槽：目录文件追加一块新空间（balloc 已清零 = 全是空槽），把名字放第一个 */
    struct inode tmp = di;
    u32 fblk = di.size / FS_BLOCK;
    u32 b = bmap(&tmp, fblk, 1);
    if (!b) return -1;
    di = tmp;
    de.inum = (u16)ino;
    fs_memset(de.name, 0, sizeof(de.name));
    fs_memcpy(de.name, name, fs_strlen(name));
    /* ⚠️ 绝不能 bwrite(b, &de)：bwrite 要写满 4KB，&de 才 64B，会把栈上邻居一起写进盘块 */
    u8 *nblk = fs_scratch;
    fs_memset(nblk, 0, FS_BLOCK);
    fs_memcpy(nblk, &de, sizeof(de));
    bwrite(b, nblk);
    di.size += FS_BLOCK;
    di.mtime = tick / 10;
    iput(dino, &di);
    return 0;
}

/* 拆路径：最后一个 '/' 之前给 parent（就地截断），之后给 name */
static int split_path(char *path, int *pino, char *name)
{
    u32 len = fs_strlen(path);
    if (len < 2 || path[0] != '/') return -1;
    while (len > 1 && path[len - 1] == '/') path[--len] = 0;   /* 去尾部斜杠 */
    u32 i = len;
    while (i > 0 && path[i - 1] != '/') i--;
    if (i == 0) return -1;
    if (i == 1) { *pino = FS_ROOT; }                          /* /xxx 的父目录就是根 */
    else {
        path[i - 1] = 0;                                      /* 截断成父路径 */
        *pino = fs_lookup(path);
        path[i - 1] = '/';
    }
    if (*pino < 0 || !fs_isdir(*pino)) return -1;
    /* ⚠️ 先卡长度再拷贝（2026-10-28 修）：调用方传进来的 name 缓冲只有 64B，
       曾经"拷完再 name_ok 拒绝"——末段名 ≥64 字节时 name[64]=0 直接把栈写穿。
       >61 本来就会被 name_ok 拒掉，提前拒只是把顺序调过来，合法路径行为不变。 */
    if (len - i > sizeof(((struct dirent *)0)->name) - 1) return -1;
    fs_memcpy(name, path + i, len - i);
    name[len - i] = 0;
    return name_ok(name, sizeof(((struct dirent *)0)->name) - 1) ? 0 : -1;
}

/* ---------------- 释放 inode 的全部资源 ---------------- */
static void ifree(int ino)
{
    struct inode in;
    iget(ino, &in);
    for (int i = 0; i < 10; i++)
        if (in.direct[i]) bfree((u32)in.direct[i]);
    if (in.indirect) {
        bread(in.indirect, fs_scratch);
        u32 *ib = (u32 *)fs_scratch;
        for (u32 i = 0; i < FS_BLOCK / 4; i++)
            if (ib[i]) bfree(ib[i]);
        bfree((u32)in.indirect);
    }
    fs_memset(&in, 0, sizeof(in));               /* type=0 = 空闲 inode */
    iput(ino, &in);
}

/* ============================================================
 * 公共 API
 * ============================================================ */
static void fs_format_now(void);   /* 后面定义（内存盘/磁盘共用） */

/* 内存盘：1MB，每次开机重新格式化（ISO/无盘运行时的行为） */
void fs_init(void)
{
    fs_on_disk = 0;
    fs_part_lba = 0;
    fs_nblocks = FS_BLOCKS;
    fs_memset(fs_disk, 0, sizeof(fs_disk));
    fs_format_now();
}

/* 在磁盘分区上格式化（安装器调用）。返回 0 成功 */
int fs_format_disk(u64 part_lba, u32 nblocks)
{
    if (nblocks < FS_B_DATA + 4) return -1;
    if (nblocks > 32768u) nblocks = 32768u;      /* 位图一块最多管 32768 块 */
    fs_on_disk = 1;
    fs_part_lba = part_lba;
    fs_nblocks = nblocks;
    fs_io_err = 0;
    fs_format_now();
    return fs_io_err ? -2 : 0;
}

/* 尝试挂载磁盘上已有的文件系统（校验 magic/块大小/块数）。返回 0 = 可用 */
int fs_mount_disk(u64 part_lba, u32 nblocks)
{
    if (nblocks < FS_B_DATA + 4) return -1;
    if (nblocks > 32768u) nblocks = 32768u;
    u8 *blk = fs_scratch;
    if (vb_read_block(FS_B_SUPER, part_lba, blk) != 0) return -1;
    struct super sb;
    fs_memcpy(&sb, blk, sizeof(sb));
    if (sb.magic != FS_MAGIC || sb.block_size != FS_BLOCK) return -2;
    if (sb.total_blocks != nblocks) return -3;   /* 分区大小变了：不冒进，交给上层重建 */
    fs_on_disk = 1;
    fs_part_lba = part_lba;
    fs_nblocks = nblocks;
    fs_io_err = 0;
    return 0;
}

int fs_is_disk(void)   { return fs_on_disk; }
u32 fs_blocks(void)    { return fs_nblocks; }
int fs_last_io_err(void) { return fs_io_err; }

/* 在当前后端上格式化（内存盘或磁盘分区都走这条） */
static void fs_format_now(void)
{
    struct super sb = { FS_MAGIC, FS_BLOCK, fs_nblocks, FS_NINODE, FS_ROOT, 0 };
    /* ASCII 进度标记：磁盘上格式化很慢时用来定位卡在哪一步（测试脚本认这些行） */
    if (fs_on_disk) { extern void kputs(const char *); kputs("[fsfmt] start\n"); }
    u8 *blk = fs_scratch;                   /* ⚠️ bwrite 永远写整块：短结构体先垫进 scratch */
    fs_memset(blk, 0, FS_BLOCK);
    fs_memcpy(blk, &sb, sizeof(sb));
    bwrite(FS_B_SUPER, blk);
    /* inode 表 4 块先清零： ialloc 是"读块→改→写回"，脏块会带出垃圾 inode */
    for (u32 b = FS_B_ITABLE; b < FS_B_DATA; b++) bwrite(b, fs_zero);
    /* 位图块也清零（2026-10-28 修）：磁盘上重新格式化时，旧位图会带着旧占用状态进来
       → 新盘一开机就"半满"甚至"已满"（数据块位全 1、balloc 立刻盘满）。
       先把位图块清零再 bmp_load，保证每次格式化从干净位图开始。 */
    bwrite(FS_B_BITMAP, fs_zero);
    bmp_load();
    for (u32 b = 0; b < FS_B_DATA; b++) bmp_set(b);   /* 元数据块先占住 */
    bmp_save();

    int root = ialloc(FS_T_DIR, 0);
    dir_add(root, ".", root);
    dir_add(root, "..", root);

    const char *dirs[] = { "etc", "bin", "home" };
    int dino[3];
    for (int i = 0; i < 3; i++) {
        dino[i] = ialloc(FS_T_DIR, 0);
        dir_add(root, dirs[i], dino[i]);
        dir_add(dino[i], ".", dino[i]);
        dir_add(dino[i], "..", root);
    }

    /* 演示文件（uid 0 root 属主） */
    int f1 = ialloc(FS_T_FILE, 0);
    dir_add(dino[0], "hostname", f1);
    const char *v1 = "myos\n";
    fs_setfile(f1, (const u8 *)v1, fs_strlen(v1));

    if (fs_on_disk) { extern void kputs(const char *); kputs("[fsfmt] meta-done\n"); }
    int f2 = ialloc(FS_T_FILE, 0);
    dir_add(dino[2], "readme.txt", f2);
    const char *v2 =
        "你好，这是灵珑系统（LoongOS）文件系统里的第一个文件！\n"
        "内存盘 1MB，块 4KB，superblock+位图+inode 表的经典结构。\n"
        "试试: ls /home  cat /home/readme.txt  write /home/a.txt hi  df\n";
    fs_setfile(f2, (const u8 *)v2, fs_strlen(v2));
}

int fs_lookup(const char *path)
{
    if (!path || path[0] != '/') return -1;
    int ino = FS_ROOT;
    char comp[64];
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        u32 n = 0;
        while (*p && *p != '/' && n < sizeof(comp) - 1) comp[n++] = *p++;
        comp[n] = 0;
        if (n == 0) break;
        if (fs_strcmp(comp, ".") == 0) continue;
        ino = dirlookup(ino, comp, 0);
        if (ino < 0) return -1;
    }
    return ino;
}

int fs_filetype(int ino)
{
    if (ino < 1 || ino >= (int)FS_NINODE) return -1;
    struct inode in;
    iget(ino, &in);
    return in.type == FS_T_NONE ? -1 : in.type;
}
int fs_isdir(int ino) { return fs_filetype(ino) == FS_T_DIR; }

int fs_filesize(int ino)
{
    if (ino < 1 || ino >= (int)FS_NINODE) return -1;
    struct inode in;
    iget(ino, &in);
    return in.type == FS_T_NONE ? -1 : (int)in.size;
}

void fs_list(int ino, void (*cb)(const struct dirent *, int, void *), void *arg)
{
    struct inode di;
    if (ino < 1 || !cb) return;
    iget(ino, &di);
    if (di.type != FS_T_DIR) return;
    struct dirent de;
    for (u32 i = 0; (i * sizeof(de)) < di.size; i++) {
        if (fread_bytes(&di, i * sizeof(de), &de, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum != 0) cb(&de, de.inum, arg);
    }
}

int fs_readfile(int ino, u8 *buf, u32 cap)
{
    if (ino < 1) return -1;
    struct inode in;
    iget(ino, &in);
    if (in.type != FS_T_FILE) return -1;
    /* 私密文件（FM_PRIVATE）：只有属主和 root 能读 */
    if ((in.mode & FM_PRIVATE) && !can_mutate(&in)) return FS_DENIED;
    int n = fread_bytes(&in, 0, buf, cap);
    return n;
}

int fs_createfile(const char *path)
{
    char tmp[128];
    u32 len = fs_strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    fs_memcpy(tmp, path, len + 1);
    int pino; char name[64];
    if (split_path(tmp, &pino, name) < 0) return -1;
    if (dirlookup(pino, name, 0) >= 0) return -1;
    /* 建文件 = 改父目录：父目录属主或 root 才行 */
    {
        struct inode pd;
        iget(pino, &pd);
        if (pd.type != FS_T_DIR || !can_mutate(&pd)) return FS_DENIED;
    }
    int ino = ialloc(FS_T_FILE, (u16)fs_uid);
    if (ino < 0) return -1;
    if (dir_add(pino, name, ino) < 0) { ifree(ino); return -1; }
    return ino;
}

int fs_mkdir(const char *path)
{
    char tmp[128];
    u32 len = fs_strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    fs_memcpy(tmp, path, len + 1);
    int pino; char name[64];
    if (split_path(tmp, &pino, name) < 0) return -1;
    if (dirlookup(pino, name, 0) >= 0) return -1;
    /* 建目录 = 改父目录：父目录属主或 root 才行 */
    {
        struct inode pd;
        iget(pino, &pd);
        if (pd.type != FS_T_DIR || !can_mutate(&pd)) return FS_DENIED;
    }
    int ino = ialloc(FS_T_DIR, (u16)fs_uid);
    if (ino < 0) return -1;
    if (dir_add(pino, name, ino) < 0) { ifree(ino); return -1; }
    dir_add(ino, ".", ino);
    dir_add(ino, "..", pino);
    return ino;
}

int fs_setfile(int ino, const u8 *buf, u32 len)
{
    if (ino < 1) return -1;
    struct inode in;
    iget(ino, &in);
    if (in.type != FS_T_FILE) return -1;
    if (!can_mutate(&in)) return FS_DENIED;    /* 改内容 = 属主或 root（阶段5） */

    /* 重写顺序（2026-10-28 修，原来是"先释放旧块再写新块"）：
       ①旧块引用先留着 → ②分配并写入新块 → ③iput 落盘新引用 → ④最后才释放旧块。
       好处：写到一半盘满/出错时旧引用原封不动（盘上 inode 从没被改过），文件完好；
       老写法一上来就释放旧块，中途失败 = 文件直接报废。
       代价：重写大文件时新旧块短暂共存，峰值多占一份空间（教学 FS 可接受）。 */
    u32 old_direct[10];
    u32 old_indirect = (u32)in.indirect;
    for (int i = 0; i < 10; i++) { old_direct[i] = (u32)in.direct[i]; in.direct[i] = 0; }
    in.indirect = 0;
    in.size = 0;

    /* 写入 */
    u32 off = 0;
    while (off < len) {
        u32 fblk = off / FS_BLOCK;
        u32 b = bmap(&in, fblk, 1);
        if (!b) {
            /* 回滚：只释放这半截新块；旧引用没动过、盘上 inode 也没写过 → 文件原样 */
            for (int i = 0; i < 10; i++) if (in.direct[i]) bfree((u32)in.direct[i]);
            if (in.indirect) {
                bread(in.indirect, fs_scratch);
                u32 *ib = (u32 *)fs_scratch;
                for (u32 i = 0; i < FS_BLOCK / 4; i++) if (ib[i]) bfree(ib[i]);
                bfree((u32)in.indirect);
            }
            return -1;
        }
        u8 *blk = fs_scratch;               /* bmap 用完 scratch 之后才填充内容 */
        u32 chunk = len - off;
        if (chunk > FS_BLOCK) chunk = FS_BLOCK;
        fs_memset(blk, 0, FS_BLOCK);
        fs_memcpy(blk, buf + off, chunk);
        bwrite(b, blk);
        off += chunk;
    }
    in.size = len;
    in.mtime = tick / 10;
    iput(ino, &in);                          /* 新引用先落盘 */

    /* ④释放旧块：新块绝不会和旧块重号——旧块一直占着位图，balloc 挑不到它们 */
    for (int i = 0; i < 10; i++) if (old_direct[i]) bfree(old_direct[i]);
    if (old_indirect) {
        bread(old_indirect, fs_scratch);
        u32 *ib = (u32 *)fs_scratch;
        for (u32 i = 0; i < FS_BLOCK / 4; i++) if (ib[i]) bfree(ib[i]);
        bfree(old_indirect);
    }
    return 0;
}

int fs_unlink(const char *path)
{
    char tmp[128];
    u32 len = fs_strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    fs_memcpy(tmp, path, len + 1);
    int pino; char name[64];
    if (split_path(tmp, &pino, name) < 0) return -1;
    if (fs_strcmp(name, ".") == 0 || fs_strcmp(name, "..") == 0) return -1;
    int slot;
    int ino = dirlookup(pino, name, &slot);
    if (ino < 0) return -1;
    struct inode in;
    iget(ino, &in);
    if (!can_mutate(&in)) return FS_DENIED;    /* 删 = 属主或 root（阶段5） */
    if (in.type == FS_T_DIR) {
        /* 空目录检查：只允许 . 和 .. 两项 */
        struct dirent de;
        for (u32 i = 0; (i * sizeof(de)) < in.size; i++) {
            if (fread_bytes(&in, i * sizeof(de), &de, sizeof(de)) != (int)sizeof(de)) break;
            if (de.inum == 0) continue;
            if (fs_strcmp(de.name, ".") != 0 && fs_strcmp(de.name, "..") != 0)
                return -1;                     /* 非空，拒绝 */
        }
    }
    /* 顺序讲究（2026-10-28 修，原来 ifree 在前）：必须先抹掉父目录里的目录项并落盘，
       再释放 inode。反过来的话，抹目录项中途出错/断电 → 目录项还指向已释放的 inode，
       它马上会被下一次 ialloc 复用，旧名字就"诈尸"指向别人的文件。 */
    struct inode di;
    iget(pino, &di);
    struct dirent de;
    de.inum = 0;
    fs_memset(de.name, 0, sizeof(de.name));
    struct inode tmpi = di;
    u32 b = bmap(&tmpi, (u32)slot * sizeof(de) / FS_BLOCK, 0);
    di = tmpi;
    if (b) {
        u8 *blk = fs_scratch;
        bread(b, blk);
        fs_memcpy(blk + (u32)slot * sizeof(de) % FS_BLOCK, &de, sizeof(de));
        bwrite(b, blk);
    }
    di.mtime = tick / 10;
    iput(pino, &di);
    ifree(ino);
    return 0;
}

void fs_statfs(u32 *used, u32 *total)
{
    u32 n = 0;
    bmp_load();
    for (u32 b = 0; b < fs_nblocks; b++)
        if (bmp_get(b)) n++;
    if (used) *used = n;
    if (total) *total = fs_nblocks;
}

/* ---------------- 阶段5：身份与权限 API ---------------- */
int fs_chown(int ino, int uid)
{
    if (ino < 1 || uid < 0 || uid > 65535) return -1;
    struct inode in;
    iget(ino, &in);
    if (in.type == FS_T_NONE) return -1;
    in.uid = (u16)uid;
    iput(ino, &in);
    return 0;
}

int fs_chmod(int ino, int mode)
{
    if (ino < 1) return -1;
    struct inode in;
    iget(ino, &in);
    if (in.type == FS_T_NONE) return -1;
    in.mode = (u8)(mode & 0xFF);
    iput(ino, &in);
    return 0;
}

int fs_inode_uid(int ino)
{
    if (ino < 1 || ino >= (int)FS_NINODE) return -1;
    struct inode in;
    iget(ino, &in);
    return in.type == FS_T_NONE ? -1 : (int)in.uid;
}
