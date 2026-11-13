/* LoongOS 第三十六轮 · 路径纯函数宿主单测（修安全审计 H1）
 *
 * 编译（在本体目录）：
 *   cc -O1 -g -Wall -Wextra -Werror test_path_host.c -o /tmp/test_path_host && /tmp/test_path_host
 *
 * 覆盖：path_join 常规/根/in-place/精确边界/超长拒绝（含 canary 越界哨兵）、
 *       path_parent 各级回退、以及「90 字节路径 + 61 字节目录名」这一 H1 真实触发场景。
 * 任何一项失败以非 0 退出，可接进 CI。
 */
#include <stdio.h>
#include <string.h>
#include "path.h"

static int ok = 0, fail = 0;

#define CHECK(cond, name) do {                                        \
    if (cond) { ok++; printf("  [ok] %s\n", name); }                 \
    else      { fail++; printf("  [FAIL] %s  (line %d)\n", name, __LINE__); } \
} while (0)

/* cap 之后的哨兵区：验证 path_join 超长拒绝时一个字节都不外溢 */
#define GUARD 16
static int canary_clean(const char *buf, int cap)
{
    for (int i = cap; i < cap + GUARD; i++)
        if ((unsigned char)buf[i] != 0xA5) return 0;
    return 1;
}
static void canary_fill(char *buf, int cap)
{
    for (int i = cap; i < cap + GUARD; i++) buf[i] = (char)0xA5;
}

int main(void)
{
    char b[PATH_CAP + GUARD];

    printf("== path_join 常规拼接 ==\n");
    {
        memset(b, 0, sizeof b); canary_fill(b, PATH_CAP);
        int n = path_join(b, PATH_CAP, "/etc", "passwd");
        CHECK(n == 11 && strcmp(b, "/etc/passwd") == 0, "普通父目录拼接");
        CHECK(canary_clean(b, PATH_CAP), "常规拼接不越界");

        n = path_join(b, PATH_CAP, "/", "home");
        CHECK(n == 5 && strcmp(b, "/home") == 0, "根父目录不产生双斜杠");

        n = path_join(b, PATH_CAP, "/a/b", "");
        CHECK(n == 5 && strcmp(b, "/a/b/") == 0, "空 child 仍合法");
    }

    printf("== path_join in-place（H1 的 fm_full(fm_path,...) 场景）==\n");
    {
        memset(b, 0, sizeof b); canary_fill(b, PATH_CAP);
        strncpy(b, "/home", PATH_CAP - 1);
        int n = path_join(b, PATH_CAP, b, "demo");   /* out == parent */
        CHECK(n == 10 && strcmp(b, "/home/demo") == 0, "同址追加结果正确");
        CHECK(canary_clean(b, PATH_CAP), "同址追加不越界");
    }

    printf("== path_join 精确边界 ==\n");
    {
        /* parent 40 字节 + child 20 字节 → need = 40+1+20+1 = 62，
           远小于测试缓冲 PATH_CAP+GUARD，便于精确验证 cap 边界。 */
        char par[64], child[32];
        memset(par, 'a', sizeof par); par[0] = '/'; par[40] = 0;   /* 40 字节 */
        memset(child, 'c', sizeof child); child[20] = 0;            /* 20 字节 */
        int need = 40 + 1 + 20 + 1;                                 /* = 62 */
        memset(b, 0, sizeof b); canary_fill(b, need);
        int n = path_join(b, need, par, child);
        CHECK(n == need - 1, "恰好等于 cap 时成功");
        CHECK(canary_clean(b, need), "等于 cap 时不写到 cap 之外");

        /* 少 1 字节：必须拒绝，且 out 一个字节都不写 */
        memset(b, 0x7E, sizeof b); canary_fill(b, need - 1);
        unsigned char before = (unsigned char)b[0];
        n = path_join(b, need - 1, par, child);
        CHECK(n == -1, "cap 差 1 字节时返回 -1");
        CHECK((unsigned char)b[0] == before, "拒绝时 out 未被改写");
        CHECK(canary_clean(b, need - 1), "拒绝时哨兵完好");
    }

    printf("== H1 真实触发场景：深目录 + 最长目录项名 ==\n");
    {
        /* fs.h dirent.name[62] → 名字最长 61 字节；路径逐层逼近 96 字节上限 */
        char deep[PATH_CAP], name[64];
        memset(deep, 0, sizeof deep);
        deep[0] = '/';
        for (int i = 1; i < 90; i++) deep[i] = 'd';   /* 90 字节父路径 */
        memset(name, 'n', 61); name[61] = 0;
        memset(b, 0, sizeof b); canary_fill(b, PATH_CAP);

        /* 旧代码：fm_full 把 90+1+61+1=153 字节写进 96 字节缓冲 → 越界。
           新代码用统一 PATH_CAP=160 应能容纳，且绝不越过 cap。 */
        int n = path_join(b, PATH_CAP, deep, name);
        CHECK(n == 152 && b[152] == 0, "153 字节路径完整落在 160 缓冲内");
        CHECK(canary_clean(b, PATH_CAP), "H1 场景无越界写");

        /* 若把目标故意缩回旧的 96 字节，必须干净拒绝（不能像旧代码那样硬写） */
        memset(b, 0x7E, sizeof b); canary_fill(b, 96);
        n = path_join(b, 96, deep, name);
        CHECK(n == -1, "目标仍为 96 字节时拒绝");
        CHECK(canary_clean(b, 96), "96 字节目标哨兵完好（不再被踩穿）");
    }

    printf("== path_parent 回退 ==\n");
    {
        memset(b, 0, sizeof b); canary_fill(b, PATH_CAP);
        CHECK(path_parent(b, PATH_CAP, "/a/b/c") == 4 && strcmp(b, "/a/b") == 0, "三级回退");
        CHECK(path_parent(b, PATH_CAP, "/a") == 1 && strcmp(b, "/") == 0, "一级回退到根");
        CHECK(path_parent(b, PATH_CAP, "/") == 1 && strcmp(b, "/") == 0, "根保持为根");
        strncpy(b, "/x/y", PATH_CAP - 1);
        CHECK(path_parent(b, PATH_CAP, b) == 2 && strcmp(b, "/x") == 0, "同址回退");
        CHECK(path_parent(b, PATH_CAP, "relative") == -1, "非绝对路径拒绝");
        CHECK(canary_clean(b, PATH_CAP), "回退不越界");
    }

    printf("== 非法参数 ==\n");
    {
        CHECK(path_join(b, 0, "/", "x") == -1, "cap=0 拒绝");
        CHECK(path_join(b, PATH_CAP, "/", (const char *)0) == -1, "child 空指针拒绝");
        CHECK(path_parent(b, PATH_CAP, "") == -1, "空路径拒绝");
    }

    printf("\n%d/%d 通过\n", ok, ok + fail);
    return fail ? 1 : 0;
}
