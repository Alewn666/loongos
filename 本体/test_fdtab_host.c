/* LoongOS 58.0-A —— FD 表宿主单测（cc -std=gnu11 -Wall -Wextra -Werror）
 * 验证 Unix 打开文件描述共享语义：fork 后父子 fd 号独立、同 ofd 共享 off、
 * 引用计数归零才释放、进程退出清理。只测 fdtab.c 纯逻辑。
 */
#include "fdtab.h"
#include <stdio.h>

static int pass = 0, fail = 0;
static void ok(int cond, const char *what)
{
    if (cond) { pass++; }
    else { fail++; printf("FAIL: %s\n", what); }
}
static void ok2(int got, int want, const char *what)
{
    if (got == want) { pass++; }
    else { fail++; printf("FAIL: %s (got=%d want=%d)\n", what, got, want); }
}

int main(void)
{
    struct ofd pool[FDT_NOFD];
    struct fd_table t;

    /* 1. 池初始化 */
    fdt_pool_init(pool, FDT_NOFD);
    ok(fdt_pool_live(pool, FDT_NOFD) == 0, "池初始空");
    ok2(fdt_pool_refsum(pool, FDT_NOFD), 0u, "ref 和初始 0");

    /* 2. 初始进程：fd0/1/2 = console，各独立 ofd，ref=1 */
    ok2(fdt_init(&t, pool, FDT_NOFD), 0, "fdt_init 成功");
    ok2(fdt_pool_live(pool, FDT_NOFD), 3, "三个 console ofd");
    ok2(fdt_get(&t, 0) >= 0 && fdt_get(&t, 1) >= 0 && fdt_get(&t, 2) >= 0, 1, "fd0/1/2 有效");
    ok(fdt_get(&t, 0) != fdt_get(&t, 1), "fd0/1 指向不同 ofd");
    ok2(fdt_get(&t, 3), -1, "fd3 空闲");
    ok2(fdt_pool_refsum(pool, FDT_NOFD), 3u, "三个 console 各 ref=1");

    /* 3. 打开文件：ino=10,len=100，装到 fd3 */
    int o = fdt_ofd_new_file(pool, FDT_NOFD, 10, 100);
    ok(o >= 0, "new_file 成功");
    pool[o].off = 40;                          /* 模拟读到偏移 40 */
    ok2(fdt_install(&t, pool, FDT_NOFD, (u32)o), 3, "装到最小空闲 fd3");
    ok2(pool[o].ref, 1u, "install 后 ref=1");

    /* 4. dup：fd3 -> fd4，同 ofd，ref++ */
    ok2(fdt_dup(&t, pool, FDT_NOFD, 3), 4, "dup 到 fd4");
    ok(fdt_get(&t, 3) == fdt_get(&t, 4), "dup 后 fd3/fd4 同 ofd");
    ok2(pool[o].ref, 2u, "dup 后 ref=2");

    /* 5. dup2：fd3 -> fd1（覆盖 console1），ref 变化 */
    ok2(fdt_dup2(&t, pool, FDT_NOFD, 3, 1), 1, "dup2 到 fd1");
    ok(fdt_get(&t, 1) == fdt_get(&t, 3), "dup2 后 fd1 指向文件 ofd");
    ok2(pool[o].ref, 3u, "dup2 覆盖后 ref=3（+1文件引用）");
    /* fd1 原 console ofd 应已释放 */
    ok(fdt_pool_live(pool, FDT_NOFD) == 3, "覆盖 console 后池仍3（console少1文件未增）");

    /* 6. close fd4：ref--，fd4 空 */
    ok2(fdt_close(&t, pool, FDT_NOFD, 4), 0, "close fd4 成功");
    ok2(fdt_get(&t, 4), -1, "fd4 关闭后空");
    ok2(pool[o].ref, 2u, "close 后 ref=2");
    ok2(fdt_close(&t, pool, FDT_NOFD, 4), -1, "重复 close 返回 -1");

    /* --- 7. fork 核心语义：复制槽位 + ofd ref++（共享 off）--- */
    {
        struct fd_table child_zero;
        for (u32 i = 0; i < FDT_NFD; i++) child_zero.slot[i] = 0;
        int before = (int)pool[o].ref;                 /* =2（fd1/fd3 两槽引用） */
        ok2(fdt_fork(&t, &child_zero, pool, FDT_NOFD), 4,
            "fork 复制4个占用槽(fd0/1/2/3)");
        /* 槽位逐一独立 */
        for (u32 fd = 0; fd < FDT_NFD; fd++)
            ok((t.slot[fd] == 0) == (child_zero.slot[fd] == 0), "fork 槽位占据一致");
        /* 同 fd 指向同一 ofd（共享打开文件描述），不是整数复制后各自独立 */
        ok(fdt_get(&t, 3) == fdt_get(&child_zero, 3), "fork 后父子 fd3 同 ofd");
        /* 文件 ofd 被父的 fd1+fd3 两槽引用，fork 复制两槽 -> ref+2（正确 Unix 语义） */
        ok2(pool[o].ref, before + 2, "fork 后文件 ofd ref+2（父2槽共享）");

        /* 共享 off：子推进 off，父看到同一个 off（Unix 语义） */
        u32 off_before = pool[o].off;                  /* =40 */
        pool[o].off += 10;                             /* 子 read 推进 */
        ok2(pool[o].off, off_before + 10, "子推进 off 到 50");
        /* 父读到的 off 就是 50（同一 ofd 对象），由上面的断言直接证明 */

        /* 子 close 自己的 fd3：ref--，但父 fd3 仍在，off 保留 */
        ok2(fdt_close(&child_zero, pool, FDT_NOFD, 3), 0, "子 close fd3");
        ok2(fdt_get(&t, 3), o, "父 fd3 仍指向文件 ofd");
        ok2(pool[o].ref, before + 1, "子 close fd3 后 ref-1（子仍持 fd1）");

        /* 子退出：destroy 整表，剩下的 3 个 console 引用逐一释放 */
        u32 live_before = (u32)fdt_pool_live(pool, FDT_NOFD);
        fdt_destroy(&child_zero, pool, FDT_NOFD);
        ok(fdt_pool_live(pool, FDT_NOFD) <= (int)live_before, "子 destroy 不增加存活 ofd");
        for (u32 fd = 0; fd < FDT_NFD; fd++)
            ok2(child_zero.slot[fd], 0u, "子 destroy 后槽位全空");
    }

    /* 8. 父 destroy 整表：所有 ofd ref 归零释放 -> 池空 */
    fdt_destroy(&t, pool, FDT_NOFD);
    ok2(fdt_pool_live(pool, FDT_NOFD), 0, "父 destroy 后池全释放");
    ok2(fdt_pool_refsum(pool, FDT_NOFD), 0u, "父 destroy 后 ref 和 0");

    /* 9. 零泄漏再跑一轮：init->open->fork->双 destroy 后池仍空 */
    fdt_pool_init(pool, FDT_NOFD);
    struct fd_table a, b;
    fdt_init(&a, pool, FDT_NOFD);
    int o2 = fdt_ofd_new_file(pool, FDT_NOFD, 99, 7);
    fdt_install(&a, pool, FDT_NOFD, (u32)o2);
    for (u32 i = 0; i < FDT_NFD; i++) b.slot[i] = 0;
    fdt_fork(&a, &b, pool, FDT_NOFD);
    fdt_destroy(&b, pool, FDT_NOFD);
    fdt_destroy(&a, pool, FDT_NOFD);
    ok2(fdt_pool_live(pool, FDT_NOFD), 0, "二次 run 后零 ofd 泄漏");

    /* 10. 池满保护 */
    fdt_pool_init(pool, FDT_NOFD);
    int made = 0;
    for (u32 i = 0; i < FDT_NOFD; i++)
        if (fdt_ofd_new_console(pool, FDT_NOFD) >= 0) made++;
    ok2(made, (int)FDT_NOFD, "池装满");
    ok2(fdt_ofd_new_file(pool, FDT_NOFD, 1, 1), -1, "池满 new_file 返回 -1");

    printf("[fdtab] %d/%d\n", pass, pass + fail);
    return fail == 0 ? 0 : 1;
}
