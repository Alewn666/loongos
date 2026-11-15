/* 第五十四轮宿主单测：mm.c（页分配 + kmalloc 零泄漏）与 rtc.c（PL031 历法）
 * 编译：cc -std=gnu11 -Wall -Wextra -Werror -O2 -o /tmp/t \
 *          test_mm_rtc_host.c mm.c rtc.c && /tmp/t
 * 不调用 rtc_init/rtc_now（那两个读 0x09010000 MMIO），只测纯函数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#define LOONGOS_RTC_TYPES   /* mm.h 已提供同型 typedef，rtc.h 不必重复 */
#include "rtc.h"

static int fails = 0, total = 0;
static void ok(int cond, const char *name)
{
    total++;
    if (cond) printf("  [%02d] PASS %s\n", total, name);
    else { printf("  [%02d] FAIL %s\n", total, name); fails++; }
}

/* ---------------- mm ---------------- */
static void *HEAP;

static void test_mm(void)
{
    printf("== mm.c 动态内存地基 ==\n");
    /* ① 参数校验 */
    ok(mm_init(0, 8 << 20) == -1, "mm_init(NULL) 拒绝");
    ok(mm_init((void *)((char *)HEAP + 1), 8 << 20) == -1, "mm_init 非页对齐拒绝");
    ok(mm_init(HEAP, 4096 * 3) == -1, "mm_init 窗口过小拒绝");
    ok(mm_init(HEAP, 8 << 20) == 0, "mm_init(8MB 对齐窗口) 成功");

    /* ② 各档边界：写满请求长度，相邻块互不踩踏 */
    static const u32 edge[MM_NCLASS] = { 16, 48, 112, 240, 496, 1008, 2032, 4080 };
    void *p[MM_NCLASS];
    int allok = 1;
    for (u32 i = 0; i < MM_NCLASS; i++) {
        p[i] = kmalloc(edge[i]);
        if (!p[i]) { allok = 0; continue; }
        memset(p[i], (int)(0xA0 + i), edge[i]);
    }
    ok(allok, "8 档边界全部分配成功");
    int intact = 1;
    for (u32 i = 0; i < MM_NCLASS; i++) {
        if (!p[i]) continue;
        unsigned char *q = (unsigned char *)p[i];
        for (u32 k = 0; k < edge[i]; k++) if (q[k] != (unsigned char)(0xA0 + i)) { intact = 0; break; }
    }
    ok(intact, "8 档相邻块写模式互不越界");
    for (u32 i = 0; i < MM_NCLASS; i++) kfree(p[i]);

    /* ③ kzalloc 清零 */
    unsigned char *z = (unsigned char *)kzalloc(2000);
    int zeroed = 1;
    if (!z) zeroed = 0;
    else for (int i = 0; i < 2000; i++) if (z[i]) { zeroed = 0; break; }
    ok(zeroed, "kzalloc(2000) 返回全零");
    kfree(z);

    /* ④ 大块（>4080）连续页 */
    unsigned char *big = (unsigned char *)kmalloc(6000);
    int bigok = big ? 1 : 0;
    if (big) { for (int i = 0; i < 6000; i++) big[i] = (unsigned char)(i ^ 0x5A); }
    if (big) for (int i = 0; i < 6000; i++) if (big[i] != (unsigned char)(i ^ 0x5A)) { bigok = 0; break; }
    ok(bigok, "大块 6000B 连续页分配 + 模式校验");
    kfree(big);

    /* ⑤ 超界请求明确失败 */
    ok(kmalloc(70000) == 0, "kmalloc(>65535) 明确失败");

    /* ⑥ 重复释放 / 野指针释放：被记录，且不破坏堆 */
    u32 tp, fp, ub, pb, lv, ac, fc, ec, bfc;
    mm_stat(&tp, &fp, &ub, &pb, &lv, &ac, &fc, &ec, &bfc);
    void *one = kmalloc(64);
    kfree(one);
    kfree(one);                                   /* 重复释放 */
    kfree((void *)((char *)HEAP + (8 << 20) + 4096));  /* 窗口外 */
    u32 tp2, fp2, ub2, pb2, lv2, ac2, fc2, ec2, bfc2;
    mm_stat(&tp2, &fp2, &ub2, &pb2, &lv2, &ac2, &fc2, &ec2, &bfc2);
    ok(bfc2 - bfc == 2, "重复释放 + 野指针释放被精确记 2 次");
    ok(lv2 == 0 && ub2 == 0, "非法释放后活跃对象/字节未失配");
    ok(kmalloc_audit() == 0, "非法释放后 kmalloc_audit() 仍健康");

    /* ⑦ 1000 次确定性压力：零泄漏 + 两轮页复用 + 峰值统计 */
    ok(kmalloc_test() == 0, "kmalloc_test() 1000×2 轮零泄漏 / 页复用 / 负例全过");
    mm_stat(&tp2, &fp2, &ub2, &pb2, &lv2, &ac2, &fc2, &ec2, &bfc2);
    ok(pb2 > 0, "峰值水位 peak_bytes 已记录");
    ok(lv2 == 0 && ub2 == 0, "压测后活跃对象/字节归零");
    ok(ac2 == fc2 && lv2 == 0, "分配/释放计数整体自洽（活跃归零时两者相等）");
    printf("     [mm] total=%u free=%u used=%u peak=%u alloc=%u free_cnt=%u badfree=%u\n",
           tp2, fp2, ub2, pb2, ac2, fc2, bfc2);
}

/* ---------------- rtc ---------------- */
struct rtc_case { u64 ep; int tz; int y, m, d, hh, mi, ss, wd; const char *label; };

static void test_rtc(void)
{
    printf("== rtc.c PL031 历法 ==\n");
    /* wday 约定：0=Sun 1=Mon ... 6=Sat */
    static const struct rtc_case cs[] = {
        { 0ULL,           0,   1970, 1, 1, 0, 0, 0, 4, "epoch 0 (Thu)" },
        { 1ULL,           0,   1970, 1, 1, 0, 0, 1, 4, "epoch 1" },
        { 86400ULL,       0,   1970, 1, 2, 0, 0, 0, 5, "epoch 86400 (Fri)" },
        { 946684800ULL,   0,   2000, 1, 1, 0, 0, 0, 6, "2000-01-01 (Sat)" },
        { 1582934400ULL,  0,   2020, 2,29, 0, 0, 0, 6, "2020-02-29 闰日 (Sat)" },
        { 4102444799ULL,  0,   2099,12,31,23,59,59, 4, "2099-12-31 (Thu)" },
        { 1794746398ULL, 480,  2026,11,15,20,39,58, 0, "会话当前 GMT+8 = Sun" },
        { 1794746398ULL,   0,  2026,11,15,12,39,58, 0, "同一时刻 UTC" },
        { 1794746398ULL,-300,  2026,11,15, 7,39,58, 0, "同一时刻 UTC-5" },
    };
    for (u32 i = 0; i < sizeof(cs)/sizeof(cs[0]); i++) {
        rtc_set_tz(cs[i].tz);
        int y, m, d, hh, mi, ss, wd;
        rtc_breakdown(cs[i].ep, &y, &m, &d, &hh, &mi, &ss, &wd);
        int good = (y==cs[i].y && m==cs[i].m && d==cs[i].d &&
                    hh==cs[i].hh && mi==cs[i].mi && ss==cs[i].ss && wd==cs[i].wd);
        char buf[128];
        snprintf(buf, sizeof buf, "breakdown %s", cs[i].label);
        if (!good) printf("       got %d-%02d-%02d %02d:%02d:%02d wd=%d\n", y,m,d,hh,mi,ss,wd);
        ok(good, buf);
        /* 逆变换：把本地字段换回 epoch 应复原 */
        u64 back = rtc_epoch_of(cs[i].y, cs[i].m, cs[i].d, cs[i].hh, cs[i].mi, cs[i].ss);
        char buf2[128];
        snprintf(buf2, sizeof buf2, "epoch_of 互逆 %s", cs[i].label);
        ok(back == cs[i].ep, buf2);
    }

    /* 连续 4 年逐日互逆（含 2024 闰年 2/29），共 1461 天 */
    rtc_set_tz(0);
    int inv_ok = 1;
    u64 ep = 1704067200ULL;    /* 2024-01-01 00:00:00 UTC */
    for (int day = 0; day < 1461; day++) {
        u64 t = ep + (u64)day * 86400ULL + 13u*3600u + 47u*60u + 29u;
        int y,m,d,hh,mi,ss,wd;
        rtc_breakdown(t, &y,&m,&d,&hh,&mi,&ss,&wd);
        if (rtc_epoch_of(y,m,d,hh,mi,ss) != t) { inv_ok = 0; break; }
        if (hh!=13 || mi!=47 || ss!=29) { inv_ok = 0; break; }
    }
    ok(inv_ok, "连续 1461 天（2024 闰年）breakdown/epoch_of 逐日互逆");

    /* rtc_format 字符串 */
    rtc_set_tz(480);
    char out[64];
    rtc_format(1794746398ULL, out, sizeof out);
    printf("     [rtc] format = %s\n", out);
    ok(strcmp(out, "2026-11-15 20:39:58 Sun UTC+0800") == 0, "rtc_format 精确字符串");
    rtc_set_tz(-300);
    rtc_format(1794746398ULL, out, sizeof out);
    ok(strcmp(out, "2026-11-15 07:39:58 Sun UTC-0500") == 0, "rtc_format 负时区");
    rtc_set_tz(480);   /* 复位默认 */
}

int main(void)
{
    HEAP = aligned_alloc(4096, 8 << 20);
    if (!HEAP) { printf("aligned_alloc 失败\n"); return 2; }
    test_mm();
    test_rtc();
    free(HEAP);
    printf("== %d/%d passed ==\n", total - fails, total);
    return fails ? 1 : 0;
}
