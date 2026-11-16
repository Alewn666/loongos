/* 第五十五轮宿主单测：usysfd.c（统一 fd + pipe + dup/dup2/close 引用计数）
 * 编译：cc -std=gnu11 -Wall -Wextra -Werror -O2 -o /tmp/t \
 *          test_usysfd_host.c usysfd.c mm.c && /tmp/t
 *
 * pipe 的 ring 走第五十四轮 kmalloc，所以本单测顺带量化「pipe 用后零泄漏」：
 * 每次 ufd_reset / 全部 close 后，mm_used_bytes() 必须回落到基线。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usysfd.h"
#include "mm.h"

static int fails = 0, total = 0;
static void ok(int cond, const char *name)
{
    total++;
    if (cond) printf("  [%02d] PASS %s\n", total, name);
    else { printf("  [%02d] FAIL %s\n", total, name); fails++; }
}

static void *HEAP;
static u8  conbuf[4096];
static u32 conlen;
static void con_sink(const u8 *b, u32 n)
{
    for (u32 i = 0; i < n && conlen < sizeof conbuf; i++) conbuf[conlen++] = b[i];
}

int main(void)
{
    printf("== usysfd.c 统一 fd / pipe 对象层 ==\n");
    HEAP = aligned_alloc(4096, 8u << 20);
    ok(HEAP != 0, "宿主堆窗口分配成功");
    ok(mm_init(HEAP, 8u << 20) == 0, "mm_init 成功");

    ufd_set_console(con_sink);
    ufd_init();

    /* ① 初始状态：0/1/2 console，其余 free */
    ok(ufd_kind(0) == UFD_CONSOLE && ufd_kind(1) == UFD_CONSOLE &&
       ufd_kind(2) == UFD_CONSOLE, "fd 0/1/2 初始为 console");
    ok(ufd_kind(3) == UFD_FREE && ufd_kind(15) == UFD_FREE, "fd 3..15 初始 free");
    ok(ufd_live_pipes() == 0, "初始无活跃 pipe");

    /* ② console 输出走注入回调 */
    conlen = 0;
    ok(ufd_write(1, (const u8 *)"AB", 2) == 2 && conlen == 2 &&
       conbuf[0] == 'A' && conbuf[1] == 'B', "写 console 走注入回调");
    ok(ufd_read(0, conbuf, 4) == 0, "读 console 返回 0（输入走 argv）");

    /* ③ pipe 基本往返 */
    int p[2];
    ok(ufd_pipe(p) == 0, "pipe() 成功");
    ok(p[0] >= UFD_NEWBASE && p[1] >= UFD_NEWBASE && p[0] != p[1],
       "pipe 两端 fd 从新编号空间分配且互异");
    ok(ufd_kind(p[0]) == UFD_PIPE_R && ufd_kind(p[1]) == UFD_PIPE_W,
       "读端/写端类型正确");
    ok(ufd_live_pipes() == 1, "活跃 pipe 数为 1");
    u32 base_used = mm_used_bytes();
    ok(base_used >= UPIPE_RING, "ring 由 kmalloc 动态分配（堆已用 >= 1024B）");

    const char *msg = "loongos pipe data";
    u32 ml = (u32)strlen(msg);
    ok(ufd_write(p[1], (const u8 *)msg, ml) == (long)ml, "写端写入完整数据");
    ok(ufd_pipe_bytes(p[0]) == ml, "pipe 缓冲字节数对账");
    u8 out[64];
    memset(out, 0, sizeof out);
    ok(ufd_read(p[0], out, ml) == (long)ml && memcmp(out, msg, ml) == 0,
       "读端读回数据逐字一致");
    ok(ufd_pipe_bytes(p[0]) == 0, "读空后缓冲归零");
    ok(ufd_read(p[0], out, 8) == 0, "空 pipe 读返回 0（非阻塞）");

    /* ④ 越界保护：读端不可写、写端不可读 */
    ok(ufd_write(p[0], (const u8 *)"x", 1) == -1, "读端写入被拒");
    ok(ufd_read(p[1], out, 1) == -1, "写端读取被拒");

    /* ⑤ 环形回绕：写满 1024 → 读 300 → 再写 300 → 读回，数据连续正确 */
    u8 big[UPIPE_RING];
    for (u32 i = 0; i < UPIPE_RING; i++) big[i] = (u8)(i & 0xFF);
    ok(ufd_write(p[1], big, UPIPE_RING) == (long)UPIPE_RING, "写满整个环形缓冲");
    ok(ufd_write(p[1], big, 1) == 0, "满缓冲再写返回 0");
    u8 rb[UPIPE_RING + 8];
    memset(rb, 0, sizeof rb);
    ok(ufd_read(p[0], rb, 300) == 300, "先读 300 字节");
    ok(memcmp(rb, big, 300) == 0, "前 300 字节正确");
    ok(ufd_write(p[1], big, 300) == 300, "回绕后再写 300 字节");
    memset(rb, 0, sizeof rb);
    ok(ufd_read(p[0], rb, UPIPE_RING) == (long)UPIPE_RING, "一次读回 1024 字节");
    /* 写满 1024 → 读 300（head=300，剩 724）→ 回绕写 300（落在 ring[0..299]）。
       再读 1024 的期望序列 = big[300..1023](724B) + big[0..299](300B)。
       分界是 1024-300=724，不是 700（首版单测在这里把期望写错了）。 */
    int wrap_ok = 1;
    for (u32 i = 0; i < UPIPE_RING; i++) {
        u8 expect = (i < 724) ? big[300 + i] : big[i - 724];
        if (rb[i] != expect) { wrap_ok = 0; break; }
    }
    ok(wrap_ok, "环形回绕后数据顺序完全正确（head/tail 环绕无错位）");

    /* ⑥ dup / dup2 / close 引用计数与零泄漏 */
    ufd_reset();                       /* 从干净状态重来 */
    ok(mm_used_bytes() == 0, "ufd_reset 释放全部 ring，堆回落到 0");
    ok(ufd_live_pipes() == 0, "ufd_reset 后无活跃 pipe");
    int q[2];
    ok(ufd_pipe(q) == 0, "重建 pipe 成功");
    int d = ufd_dup(q[1]);
    ok(d >= UFD_NEWBASE && d != q[0] && d != q[1], "dup(写端) 分配新 fd");
    ok(ufd_kind(d) == UFD_PIPE_W, "dup 出的对象类型一致");
    ok(ufd_write(d, (const u8 *)"XY", 2) == 2, "dup 出的写端可写");
    ok(ufd_close(d) == 0, "close(dup fd) 成功");
    ok(ufd_live_pipes() == 1, "只关 dup 端时 pipe 仍存活（writers 尚存）");
    ok(ufd_close(q[1]) == 0, "关原始写端");
    u8 z[8];
    ok(ufd_read(q[0], z, 2) == 2 && z[0] == 'X' && z[1] == 'Y',
       "写端全关后仍可读到残留数据");
    ok(ufd_close(q[0]) == 0, "关读端");
    ok(ufd_live_pipes() == 0, "两端全关后 pipe 对象回收");
    ok(mm_used_bytes() == 0, "pipe 全关后堆零泄漏（kmalloc 地基真实消费）");

    /* ⑦ dup2 到 fd 1：stdout 重定向到 pipe 写端 */
    ufd_reset();
    int r[2];
    ok(ufd_pipe(r) == 0, "dup2 用例建 pipe");
    ok(ufd_dup2(r[1], 1) == 1, "dup2(写端, 1) 返回 1");
    ok(ufd_kind(1) == UFD_PIPE_W, "fd1 已被重定向为 pipe 写端（钩子会触发）");
    conlen = 0;
    ok(ufd_write(1, (const u8 *)"REDIR", 5) == 5, "写 fd1 进入 pipe");
    ok(conlen == 0, "重定向期间 console 回调未收到字节");
    ok(ufd_pipe_bytes(r[0]) == 5, "fd1 写入的数据落在 pipe 里");
    memset(out, 0, sizeof out);
    ok(ufd_read(r[0], out, 5) == 5 && memcmp(out, "REDIR", 5) == 0,
       "从读端取回被重定向的 stdout 内容");
    ok(ufd_close(1) == 0, "close(fd1) 成功");
    ok(ufd_kind(1) == UFD_CONSOLE, "close(fd1) 后标准流恢复 console");

    /* ⑧ 非法 fd / 资源耗尽保护 */
    ok(ufd_kind(-1) == -1 && ufd_kind(UFD_MAX) == -1, "越界 fd 查询返回 -1");
    ok(ufd_close(99) == -1, "close 越界 fd 被拒");
    ok(ufd_dup(0) >= 0, "dup(console fd0) 允许（复制 console 对象）");
    ok(ufd_dup2(0, 0) == 0, "dup2(old==new) 直接返回 new");
    ok(ufd_dup2(0, 99) == -1, "dup2 目标越界被拒");

    /* ⑨ 资源上限：连续建 8 个 pipe 后第 9 个应失败，reset 后零泄漏 */
    ufd_reset();
    int made = 0;
    for (int i = 0; i < UPIPE_MAX + 3; i++) {
        int t[2];
        if (ufd_pipe(t) == 0) made++;
    }
    ok(made == UPIPE_MAX, "pipe 对象上限 = UPIPE_MAX(8)，超出被拒");
    ok(ufd_live_pipes() == UPIPE_MAX, "活跃 pipe 数等于上限");
    ok(ufd_alloc_fail() == 0, "本用例未发生 kmalloc 失败（失败是对象/槽位耗尽）");
    ufd_reset();
    ok(mm_used_bytes() == 0 && ufd_live_pipes() == 0,
       "批量 pipe 后 ufd_reset 一次性释放，堆零泄漏");

    printf("\n== 结果：%d 过 / %d 挂 ==\n", total - fails, fails);
    return fails ? 1 : 0;
}
