/* fs.c 主机端单元测试：验证 2026-10-28 四项修复（split_path 栈溢出 / unlink 顺序 /
 * 格式化位图 / setfile 先写后放）+ 目录解析回归。
 * 编译：cc -O1 -g -fstack-protector-all -Wall test_fs_host.c fs.c -o test_fs_host
 * fs.c 纯 C 零依赖（RAM 路径走内置 fs_disk 数组），只补三个外部符号桩。 */
#include <stdio.h>
#include <string.h>
#include "fs.h"

/* —— fs.c 需要的外部符号（RAM 盘模式下 vb_* 不会被真调用，kputs 被 fs_on_disk 门住）—— */
volatile u64 tick = 0;
int vb_read_block(u64 blk_no, u64 part_lba, void *buf)
{ (void)blk_no; (void)part_lba; (void)buf; return -1; }
int vb_write_block(u64 blk_no, u64 part_lba, const void *buf)
{ (void)blk_no; (void)part_lba; (void)buf; return -1; }
void kputs(const char *s) { (void)s; }

static int fails = 0, total = 0;
static void ok(int cond, const char *name)
{
    total++;
    printf("  [%02d] %s %s\n", total, cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}

static u8 buf_a[24 * 1024], buf_b[24 * 1024], rb[24 * 1024];
static u8 big[32 * 1024];   /* 盘满回滚用：请求必须超过剩余块数，但要在合法缓冲内 */

static void filler_path(int k, char p[16])
{
    memcpy(p, "/home/f000.txt", 15);
    p[7] = (char)('0' + k / 100);
    p[8] = (char)('0' + (k / 10) % 10);
    p[9] = (char)('0' + k % 10);
}

int main(void)
{
    printf("== fs.c host test（2026-10-28 修复验证）==\n");

    /* T1 基线 */
    fs_init();
    u32 used0 = 0, totalb = 0;
    fs_statfs(&used0, &totalb);
    ok(totalb == 256, "T1 fs_init 基线（256 块）");
    ok(fs_is_disk() == 0, "T1 RAM 盘模式");

    /* T2 超长文件名：修前 name[64] 栈缓冲被写穿 */
    char longname[128];
    memcpy(longname, "/home/", 6);
    memset(longname + 6, 'a', 80);
    longname[86] = 0;
    ok(fs_lookup(longname) == -1, "T2 80 字节名 lookup 被拒（修前栈溢出）");
    ok(fs_createfile(longname) == -1, "T2 80 字节名 createfile 也被拒");

    /* T3 名字长度边界：61 字节 OK / 62 字节拒 */
    char n61[80], n62[80];
    memcpy(n61, "/home/", 6); memset(n61 + 6, 'b', 61); n61[67] = 0;
    memcpy(n62, "/home/", 6); memset(n62 + 6, 'c', 62); n62[68] = 0;
    int i61 = fs_createfile(n61);
    ok(i61 > 0, "T3 61 字节名可建");
    ok(fs_lookup(n61) == i61, "T3 61 字节名可查");
    ok(fs_createfile(n62) == -1, "T3 62 字节名被拒");

    /* T4 unlink：块回收 + inode 槽复用 */
    int ia = fs_createfile("/home/x.txt");
    ok(ia > 0, "T4 建 /home/x.txt");
    memset(buf_a, 'X', 4096);
    ok(fs_setfile(ia, buf_a, 4096) == 0, "T4 写 1 块");
    u32 used1 = 0; fs_statfs(&used1, &totalb);
    ok(fs_unlink("/home/x.txt") == 0, "T4 unlink 成功");
    ok(fs_lookup("/home/x.txt") == -1, "T4 删后查不到");
    u32 used2 = 0; fs_statfs(&used2, &totalb);
    ok(used2 < used1, "T4 块被回收（用量下降）");
    int ib = fs_createfile("/home/y.txt");
    ok(ib == ia, "T4 inode 槽被复用（ifree 真跑了）");

    /* T5/T8 覆盖写 3块→5块：内容对、净增恰 2 块（无泄漏） */
    for (int i = 0; i < 3 * 4096; i++) buf_a[i] = (u8)(i & 0x7f);
    ok(fs_setfile(ib, buf_a, 3 * 4096) == 0, "T5 写 3 块");
    u32 u3 = 0; fs_statfs(&u3, &totalb);
    for (int i = 0; i < 5 * 4096; i++) buf_b[i] = (u8)(0x80 | (i & 0x7f));
    ok(fs_setfile(ib, buf_b, 5 * 4096) == 0, "T5 覆盖写 5 块");
    u32 u5 = 0; fs_statfs(&u5, &totalb);
    ok(u5 - u3 == 2, "T5 净增恰 2 块（旧块全释放、无泄漏）");
    int n = fs_readfile(ib, rb, sizeof(rb));
    ok(n == 5 * 4096 && memcmp(rb, buf_b, 5 * 4096) == 0, "T8 覆盖后内容正确");

    /* T6 盘满回滚（本次修复核心）：写满盘后要求覆盖成超大文件。
       ⚠️ filler 每个占 4 块：inode 上限 128 个，若每 filler 只占 1 块会先耗尽 inode
       而不是块，盘根本没满（第一版测试就栽在这，误判成代码 bug）。 */
    int fillers = 0;
    for (int k = 0; k < 300; k++) {
        char p[16];
        filler_path(k, p);
        int fi = fs_createfile(p);
        if (fi < 0) break;
        if (fs_setfile(fi, buf_a, 4 * 4096) < 0) { fs_unlink(p); break; }
        fillers++;
    }
    ok(fillers > 40, "T6 填盘成功（>40 个 4 块 filler，盘满）");
    u32 uf = 0; fs_statfs(&uf, &totalb);
    int rr = fs_setfile(ib, big, 32 * 4096);       /* 盘上空块远不够 → 必失败 */
    ok(rr == -1, "T6 盘满时覆盖写返回 -1");
    u32 ur = 0; fs_statfs(&ur, &totalb);
    ok(ur == uf, "T6 回滚后块用量纹丝不动（半截新块已释放）");
    n = fs_readfile(ib, rb, sizeof(rb));
    ok(n == 5 * 4096 && memcmp(rb, buf_b, 5 * 4096) == 0, "T6 原文件内容完好（修前=报废）");
    ok(fs_filesize(ib) == 5 * 4096, "T6 原文件大小不变");

    /* T7 截断到 0 + 腾空后恢复大写入 */
    for (int k = 0; k < fillers; k++) {
        char p[16];
        filler_path(k, p);
        fs_unlink(p);
    }
    ok(fs_setfile(ib, buf_a, 0) == 0 && fs_filesize(ib) == 0, "T7 截断到 0");
    u32 u0 = 0; fs_statfs(&u0, &totalb);
    /* 目录块只增不减：filler 的 dirent 清零但 /home 的块保留 → 允许 ≤4 块目录余量 */
    ok(u0 >= used2 && u0 - used2 <= 4, "T7 截断后块全回收（只剩目录余量 ≤4 块）");
    ok(fs_setfile(ib, buf_b, 5 * 4096) == 0, "T7 腾空后又能写大文件");

    /* T9 目录 / '..' 解析回归 */
    ok(fs_mkdir("/home/sub") > 0, "T9 mkdir");
    int f9 = fs_createfile("/home/sub/z.txt");
    ok(f9 > 0, "T9 嵌套建文件");
    ok(fs_lookup("/home/sub/../sub/z.txt") == f9, "T9 '..' 解析");

    /* T10 重新格式化后干净可用 */
    fs_init();
    ok(fs_lookup("/home/readme.txt") > 0, "T10 重格式化演示文件在");
    int f10 = fs_createfile("/home/new.txt");
    ok(f10 > 0 && fs_setfile(f10, buf_a, 8192) == 0, "T10 重格式化后可建可写");

    printf(fails ? "== 结果：%d/%d FAIL ==\n" : "== 结果：%d/%d 全过 ==\n", total - fails, total);
    return fails ? 1 : 0;
}
