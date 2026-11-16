/* 第五十七轮 进程模型宿主单测：进程表状态机 + fork 全拷贝隔离
 * 编译：cc -std=gnu11 -Wall -Wextra -Werror -O2 -o /tmp/test_proc \
 *          test_proc_host.c proc.c && /tmp/test_proc
 */
#include <stdio.h>
#include <string.h>
typedef unsigned long      u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
#include "proc.h"

static int passed = 0, failed = 0;
static void ok(int c, const char *name)
{
    if (c) { passed++; }
    else { failed++; printf("[FAIL] %s\n", name); }
}
static void ok2(int c, const char *name, int a, int b)
{
    if (c) { passed++; }
    else { failed++; printf("[FAIL] %s (a=%d b=%d)\n", name, a, b); }
}

/* ---- 用两个数组模拟父/子用户空间，验证读写回调注入的全拷贝 ---- */
#define VMEM 8192
static u8 parent_vm[VMEM], child_vm[VMEM];
static int rd_va(u64 va, void *buf, u32 n, void *ctx)
{
    u8 *mem = (u8 *)ctx;
    if ((u64)n > VMEM || va > (u64)(VMEM - n)) return -1;
    memcpy(buf, mem + va, n);
    return 0;
}
static int wr_va(u64 va, void *buf, u32 n, void *ctx)
{
    u8 *mem = (u8 *)ctx;
    if ((u64)n > VMEM || va > (u64)(VMEM - n)) return -1;
    memcpy(mem + va, buf, n);
    return 0;
}

int main(void)
{
    struct proc tab[NPROC];

    /* 1. 初始化 */
    proc_init(tab, NPROC);
    ok(tab[0].state == P_FREE && tab[0].pid == 0, "init 清零槽位0");
    ok(tab[NPROC-1].state == P_FREE, "init 清零末槽");

    /* 2. 分配：pid 从 2 起，父为 1 */
    int p2 = proc_alloc(tab, NPROC, 1);
    int p3 = proc_alloc(tab, NPROC, 1);
    ok2(p2 == 2, "fork 首个子 pid=2", p2, 2);
    ok2(p3 == 3, "fork 次个子 pid=3", p3, 3);
    ok(proc_find(tab, NPROC, 2) == &tab[0], "find 定位槽位0");
    ok(proc_find(tab, NPROC, 99) == 0, "find 不存在的 pid 返回0");

    /* 3. 表满：NPROC=4 → 槽位 2..3 给 pid 4,5，再分配失败 */
    int p4 = proc_alloc(tab, NPROC, 1);
    int p5 = proc_alloc(tab, NPROC, 1);
    int pfull = proc_alloc(tab, NPROC, 1);
    ok2(p4 == 4, "填满槽位 pid=4", p4, 4);
    ok2(p5 == 5, "填满槽位 pid=5", p5, 5);
    ok2(pfull == -1, "表满返回-1", pfull, -1);

    /* 4. 僵尸状态机 */
    int zr = proc_zombie(tab, NPROC, 3, 7);
    ok2(zr == 0, "pid3 僵尸化 code=7", zr, 0);
    ok(proc_find(tab, NPROC, 3)->state == P_ZOMBIE, "pid3 状态 ZOMBIE");
    ok(proc_zombie(tab, NPROC, 3, 9) == -1, "重复僵尸化被拒");
    ok(proc_zombie(tab, NPROC, 99, 0) == -1, "不存在的 pid 僵尸化被拒");
    ok2(proc_zombie_count(tab, NPROC, 1) == 1, "父1 名下有1个僵尸", proc_zombie_count(tab, NPROC, 1), 1);
    ok(proc_zombie_count(tab, NPROC, 2) == 0, "父2 名下无僵尸");

    /* 5. 回收：写回退出码与子 pid，槽位释放 */
    int code = -1; u32 cpid = 0;
    int rr = proc_reap(tab, NPROC, 1, &code, &cpid);
    ok2(rr == 0, "回收成功", rr, 0);
    ok2(code == 7, "回收退出码=7", code, 7);
    ok2(cpid == 3, "回收子 pid=3", (int)cpid, 3);
    ok(tab[1].state == P_FREE, "回收后槽位1 释放");
    int rr2 = proc_reap(tab, NPROC, 1, &code, &cpid);
    ok2(rr2 == -1, "再回收无僵尸返回-1", rr2, -1);

    /* 6. 回收后槽位可再分配 */
    int p3again = proc_alloc(tab, NPROC, 1);
    ok2(p3again == 3, "回收后 pid3 槽位复用", p3again, 3);

    /* 7. free 负例 */
    ok2(proc_free(0) == -1, "free(NULL) 被拒", proc_free(0), -1);
    struct proc fresh; proc_init(&fresh, 1);
    ok2(proc_free(&fresh) == -1, "free 空槽被拒", proc_free(&fresh), -1);

    /* ---- 8. fork 全拷贝隔离：父写满→拷到子→子改→父不变 ---- */
    for (int i = 0; i < VMEM; i++) parent_vm[i] = (u8)(i * 7 + 3);
    memset(child_vm, 0, sizeof child_vm);
    ok2(proc_copy_userspace(0, VMEM, 64, rd_va, parent_vm, wr_va, child_vm) == 0,
        "全拷贝 8192B 成功", proc_copy_userspace(0, VMEM, 64, rd_va, parent_vm, wr_va, child_vm), 0);
    ok(memcmp(parent_vm, child_vm, VMEM) == 0, "拷贝后父子内存逐字节一致");
    child_vm[100] = 0xAB; child_vm[4096] = 0xCD;   /* 模拟子进程改自己的变量 */
    ok(parent_vm[100] != 0xAB && parent_vm[4096] != 0xCD, "子改变量后父内存不受影响（地址隔离）");

    /* 9. 拷贝边界/负例：chunk 钳到 256、越界读写被拒 */
    ok2(proc_copy_userspace(VMEM - 8, 8, 64, rd_va, parent_vm, wr_va, child_vm) == 0,
        "尾部小块拷贝成功", proc_copy_userspace(VMEM - 8, 8, 64, rd_va, parent_vm, wr_va, child_vm), 0);
    ok2(proc_copy_userspace(VMEM - 4, 16, 64, rd_va, parent_vm, wr_va, child_vm) == -1,
        "越界读被拒", proc_copy_userspace(VMEM - 4, 16, 64, rd_va, parent_vm, wr_va, child_vm), -1);
    ok2(proc_copy_userspace(0, 8, 0, rd_va, parent_vm, wr_va, child_vm) == -1,
        "chunk=0 被拒", proc_copy_userspace(0, 8, 0, rd_va, parent_vm, wr_va, child_vm), -1);

    /* 10. 帧缓冲 800B 可被整体保存/恢复（上下文切换的载体） */
    u8 frame_a[PROC_FRAME_BYTES], frame_b[PROC_FRAME_BYTES];
    for (u32 i = 0; i < PROC_FRAME_BYTES; i++) frame_a[i] = (u8)(i ^ 0x5A);
    memcpy(tab[0].frame, frame_a, PROC_FRAME_BYTES);   /* 换出：存帧 */
    memcpy(frame_b, tab[0].frame, PROC_FRAME_BYTES);   /* 换入：取帧 */
    ok(memcmp(frame_a, frame_b, PROC_FRAME_BYTES) == 0, "800B 异常帧整体保存/恢复一致");

    printf("\n[test_proc] %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
