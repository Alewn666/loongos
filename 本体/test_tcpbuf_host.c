/* tcp.c 接收缓冲读写游标一致性宿主单测（回归用）
 *
 * 复现真实网络下「大包收不全」根因：tcp_rx 写入用 t_rx+t_rx_len，
 * tcp_recv 读取用 t_rx+t_rx_head。当应用读走一部分使 head>0 后，
 * 新段写到 t_rx+len 会落进合法未读区 [head, head+len) 内部，
 * 覆盖尚未读走的字节 —— SLIRP 小段每次读空(head 归 0)掩盖了它，
 * 真机满 MSS 段让 head 持续>0 时必现，表现为 HTTP body 被改写。
 *
 * 编译：cc -O1 -g -Wall -DNET_NO_CNT test_tcpbuf_host.c -o /tmp/test_tcpbuf_host
 * 预期：修复前 FAIL（数据被覆盖），修复后 PASS。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NET_NO_CNT 1
#include "tcp.c"          /* 静态函数/状态全可见；外部符号由下面桩提供 */

/* ---- 桩 ---- */
volatile u64 tick = 1234;             /* tcp.c 多处引用（TIME_WAIT 时间戳） */
u64 cnt_read(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
u64 cnt_freq(void) { return 1000000000ull; }
u32 net_my_ip(void) { return 0x0a00020a; }        /* 10.0.2.10 */
int ip_send_tcp(u32 d, const u8 *s, int l) { (void)d; (void)s; (void)l; return 0; }
void net_poll(void) { }

/* 组TCP段喂给 tcp_rx；自动按 MSS 拆分并按序推进 seq。
   payload 内容由 fill 回调给出（下标->字节值），便于校验覆盖。 */
static void feed_seq(int total, u8 (*fill)(int))
{
    int sent = 0;
    while (sent < total) {
        int n = total - sent; if (n > TCP_MSS) n = TCP_MSS;
        u8 pkt[20 + 20 + TCP_MSS];
        int tot = 20 + 20 + n;
        memset(pkt, 0, tot);
        pkt[0] = 0x45; pkt[2] = (u8)(tot >> 8); pkt[3] = (u8)tot;
        put32(pkt + 12, 0x0a000201);      /* src = 对端（回包源） */
        put32(pkt + 16, 0x0a00020a);      /* dst = 我方 */
        u8 *seg = pkt + 20;
        put16(seg + 0, t_rport); put16(seg + 2, t_lport);
        put32(seg + 4, t_rcv_nxt);        /* seq = 当前期望（按序） */
        put32(seg + 8, t_snd_nxt);
        seg[12] = 5 << 4; seg[13] = TCP_F_ACK | TCP_F_PSH;
        for (int i = 0; i < n; i++) seg[20 + i] = fill(sent + i);
        put16(seg + 16, tcp_cksum(get32(pkt + 12), get32(pkt + 16), seg, 20 + n));
        tcp_rx(pkt, tot);
        sent += n;
    }
}
static u8 fill_a1(int i) { (void)i; return 0xA1; }
static u8 fill_b2(int i) { (void)i; return 0xB2; }

static int fails, total;
static void ok(int cond, const char *name)
{
    total++; printf("  [%02d] %s %s\n", total, cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}

int main(void)
{
    /* 手工把连接置为 ESTAB，绕过三次握手 */
    t_state = TS_ESTAB;
    t_rip = 0x0a000201; t_myip = 0x0a00020a;
    t_rport = 80; t_lport = 45000;
    t_iss = 1000; t_snd_una = t_snd_nxt = 1001; t_rcv_nxt = 5000;
    t_rx_head = t_rx_len = 0;

    /* 段 A：3000 字节，全 0xA1（自动拆成 3 段满 MSS） */
    feed_seq(3000, fill_a1);
    ok(t_rx_len == 3000 && t_rx_head == 0, "A: 缓冲收到 3000B");

    /* 应用读走 1000B -> head=1000, len=2000 */
    u8 got[5000];
    int n1 = tcp_recv(got, 1000, 100);
    ok(n1 == 1000, "A: 读走 1000B");
    /* 修复后 tcp_recv 会归一化：head=0, len=2000（未读数据搬到缓冲头）。
       修复前是 head=1000,len=2000，那正是覆盖 bug 的触发前提。 */
    ok(t_rx_head == 0 && t_rx_len == 2000, "A: 归一化后 head=0 len=2000");

    /* 段 B：1000 字节，全 0xB2，seq 紧跟 A */
    feed_seq(1000, fill_b2);

    /* 把剩余全部读出：应为 2000 个 0xA1 接着 1000 个 0xB2 */
    int n2 = tcp_recv(got + 1000, 4000, 100);
    ok(n2 == 3000, "B: 剩余 3000B 可读");
    int bad_a1 = 0, bad_b2 = 0;
    for (int i = 0; i < 2000; i++) if (got[1000 + i] != 0xA1) bad_a1++;
    for (int i = 0; i < 1000; i++) if (got[3000 + i] != 0xB2) bad_b2++;
    printf("    剩余区前2000应0xA1：坏字节=%d；后1000应0xB2：坏字节=%d\n", bad_a1, bad_b2);
    ok(bad_a1 == 0, "关键: 段A未读的2000B未被段B覆盖");
    ok(bad_b2 == 0, "关键: 段B的1000B完整");

    printf("\n结果: %d/%d PASS\n", total - fails, total);
    return fails ? 1 : 0;
}
