/* net.c 主机端单元测试（2026-10-30，给 LoongOS 联网功能上线前把协议路径先钉死）
 *
 * 套路同 test_fs_host.c：把 net.c 整份 include 进来，静态函数直接可见；
 * 只桩掉外部符号（串口打印 / 驱动 / tick）。
 *
 * 覆盖：
 *   T1  Internet 校验和（已知向量 + 自洽性）
 *   T2  点分十进制 IP 解析/格式化（往返 + 非法输入）
 *   T3  UDP 伪首部校验和（算出来再验应为 0）
 *   T4  ICMP echo request → 收到合法 echo reply（完整以太网帧进 on_frame）
 *   T5  ARP 请求 → 应答；缓存记录/查询
 *   T6  DHCP 报文解析（OFFER/ACK 字段、xid 不匹配拒绝、op 错拒绝）
 *   T7  ip_rx 过滤（坏校验和/分片/非本机）
 *   T8  完整 DHCP 流程（桩 vn_send 扮演 DHCP 服务器：DISCOVER→OFFER→REQUEST→ACK）
 *   T9  net static 手配（合法/非法输入）
 *
 * 编译：cc -O1 -g -Wall test_net_host.c -o /tmp/test_net_host
 * ⚠️ 需要 aarch64 主机（net.c 里有 mrs cntpct_el0；NAS 就是 aarch64）
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NET_NO_CNT 1      /* 用户态读 cntpct_el0 会 SIGILL，改注入 clock_gettime */
#include "net.c"          /* 静态函数全部可见；外部符号由下面的桩提供 */

/* 注入的时钟：纳秒单调钟，频率 1e9 */
u64 cnt_read(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
u64 cnt_freq(void) { return 1000000000ull; }

/* ---------------- 桩 ---------------- */
volatile u64 tick = 1234;
static char g_log[8192];
static int  g_log_n;
void kputs(const char *s) { while (*s && g_log_n < (int)sizeof g_log - 1) g_log[g_log_n++] = *s++; g_log[g_log_n] = 0; }
void kputhex(u64 v) { char b[24]; int n = 0; if (!v) b[n++] = '0'; while (v) { int d = (int)(v & 15); b[n++] = d < 10 ? (char)('0' + d) : (char)('a' + d - 10); v >>= 4; } while (n) { char c[2] = { b[--n], 0 }; kputs(c); } }
void kputdec(u32 v) { char b[16]; int n = 0; if (!v) b[n++] = '0'; while (v) { b[n++] = (char)('0' + v % 10); v /= 10; } while (n) { char c[2] = { b[--n], 0 }; kputs(c); } }

static int vn_stub_present = 1;
int  vn_init(void) { return 0; }
int  vn_present(void) { return vn_stub_present; }
void vn_mac_copy(u8 out[6]) { static const u8 m[6] = { 0x42, 0x4f, 0xa5, 0xf9, 0xa6, 0x62 }; for (int i = 0; i < 6; i++) out[i] = m[i]; }
int  vn_link(void) { return 1; }

/* 发出的帧都记下来；可挂一个「自动扮演服务器」的回调（T8 用） */
static u8  g_tx[8][1600];
static int g_tx_len[8];
static int g_tx_n;
static void (*g_on_send)(const u8 *frame, int len);

int vn_send(const void *frame, int len)
{
    if (g_tx_n < 8) { memcpy(g_tx[g_tx_n], frame, (size_t)len); g_tx_len[g_tx_n] = len; }
    g_tx_n++;
    if (g_on_send) g_on_send((const u8 *)frame, len);
    return 0;
}
int vn_poll(int (*on_rx)(const u8 *, int)) { (void)on_rx; return 0; }

/* ---------------- 测试工具 ---------------- */
static int fails, total;
static void ok(int cond, const char *name)
{
    total++;
    printf("  [%02d] %s %s\n", total, cond ? "PASS" : "FAIL", name);
    if (!cond) fails++;
}
static void tx_reset(void) { g_tx_n = 0; }

/* 组一个完整以太网帧（给 on_frame 喂） */
static int mk_eth(u8 *f, const u8 dst[6], const u8 src[6], u16 type)
{
    memcpy(f, dst, 6); memcpy(f + 6, src, 6);
    f[12] = (u8)(type >> 8); f[13] = (u8)type;
    return 14;
}

/* 在 eth 后组 IPv4 头，返回新的总长 */
static int mk_ip(u8 *f, int off, u32 src, u32 dst, u8 proto, int payload_len)
{
    u8 *ip = f + off;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (u8)((20 + payload_len) >> 8); ip[3] = (u8)(20 + payload_len);
    ip[4] = ip[5] = 0; ip[6] = ip[7] = 0;
    ip[8] = 64; ip[9] = proto;
    ip[10] = ip[11] = 0;
    put32(ip + 12, src); put32(ip + 16, dst);
    put16(ip + 10, inet_cksum(ip, 20));
    return off + 20;
}

static int mk_icmp_echo_request(u8 *f, int off, u16 id, u16 seq, int paylen)
{
    u8 *ic = f + off;
    ic[0] = 8; ic[1] = 0; ic[2] = ic[3] = 0;
    put16(ic + 4, id); put16(ic + 6, seq);
    for (int i = 0; i < paylen; i++) ic[8 + i] = (u8)('A' + i % 26);
    put16(ic + 2, inet_cksum(ic, 8 + paylen));
    return 8 + paylen;
}

/* 组 DHCP 报文（udp_payload 部分）：op/msg_type/yiaddr/mask/gw/server */
static int mk_dhcp(u8 *p, u32 xid, u8 op, u8 msg, u32 yiaddr, u32 mask, u32 gw, u32 srv, const u8 mac[6])
{
    memset(p, 0, 548);
    p[0] = op; p[1] = 1; p[2] = 6;
    put32(p + 4, xid);
    memcpy(p + 28, mac, 6);
    put32(p + 16, yiaddr);
    put32(p + 236, 0x63825363);
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = msg;
    if (yiaddr) { p[o++] = 1; p[o++] = 4; put32(p + o, mask); o += 4; }
    if (gw)     { p[o++] = 3; p[o++] = 4; put32(p + o, gw); o += 4; }
    if (srv)    { p[o++] = 54; p[o++] = 4; put32(p + o, srv); o += 4; }
    p[o++] = 255;
    return o;
}

/* 把 DHCP 载荷封成完整以太网/IP/UDP 帧，返回总长 */
static int mk_dhcp_frame(u8 *f, u32 src_ip, u32 dst_ip, const u8 dst_mac[6], const u8 *dh, int dhlen, u16 dport)
{
    const u8 mac[6] = { 0x42, 0x4f, 0xa5, 0xf9, 0xa6, 0x62 };
    int n = mk_eth(f, dst_mac, mac, ETH_IP);
    int u = mk_ip(f, n, src_ip, dst_ip, IP_UDP, 8 + dhlen);
    u8 *udp = f + u;
    put16(udp + 0, DHCP_SERVER_PORT); put16(udp + 2, dport);
    put16(udp + 4, (u16)(8 + dhlen));
    udp[6] = udp[7] = 0;
    memcpy(udp + 8, dh, (size_t)dhlen);
    put16(udp + 6, udp_cksum(src_ip, dst_ip, udp, 8 + dhlen));
    return u + 8 + dhlen;
}

/* T8 用：扮演 DHCP 服务器的 vn_send 钩子
 * 收到 DISCOVER（option53=1）→ 回 OFFER；收到 REQUEST（option53=3）→ 回 ACK。
 * 帧布局：eth 14 + ip 20 + udp 8 = 42，bootp 从 42 开始，xid 在 +46，magic 在 +236。 */
static void dhcp_server_stub(const u8 *frame, int len)
{
    const u32 nas_ip = 0xc0a80a0du, our_ip = 0xc0a80a4du, gw_ip = 0xc0a80a01u;
    const u8 our_mac[6] = { 0x42, 0x4f, 0xa5, 0xf9, 0xa6, 0x62 };
    const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (len < 42 + 240) return;
    const u8 *dh = frame + 42;
    if (dh[0] != 1) return;                       /* 只理客户端请求 */
    if (get32(dh + 236) != 0x63825363) return;
    u32 xid = get32(dh + 4);
    /* 找 option 53 */
    int i = 240, msg = 0;
    while (i < len - 42) {
        u8 code = dh[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }
        int l = dh[i + 1];
        if (code == 53 && l >= 1) msg = dh[i + 2];
        i += 2 + l;
    }
    u8 p[548];
    u8 out[1600];
    if (msg == 1) {                               /* DISCOVER → OFFER */
        int l = mk_dhcp(p, xid, 2, 2, our_ip, 0xffffff00u, gw_ip, nas_ip, our_mac);
        int n = mk_dhcp_frame(out, nas_ip, 0xffffffffu, bcast, p, l, DHCP_CLIENT_PORT);
        on_frame(out, n);
    } else if (msg == 3) {                        /* REQUEST → ACK */
        int l = mk_dhcp(p, xid, 2, 5, our_ip, 0xffffff00u, gw_ip, nas_ip, our_mac);
        int n = mk_dhcp_frame(out, nas_ip, 0xffffffffu, bcast, p, l, DHCP_CLIENT_PORT);
        on_frame(out, n);
    }
}

/* ================= main ================= */
int main(void)
{
    const u8 our_mac[6] = { 0x42, 0x4f, 0xa5, 0xf9, 0xa6, 0x62 };
    const u8 peer_mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    const u32 nas_ip  = 0xc0a80a0du;   /* 192.168.1.100 */
    const u32 our_ip  = 0xc0a80a4du;   /* 192.168.10.77 */
    const u32 gw_ip   = 0xc0a80a01u;   /* 192.168.10.1 */
    const u32 mask    = 0xffffff00u;
    const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    printf("== net.c host test ==\n");

    /* T1 校验和 */
    {
        u8 v[2] = { 0x00, 0x01 };
        u8 w[2] = { 0xf2, 0x03 };
        u16 s = inet_cksum(v, 2);           /* 0x0001 → ~1 = 0xfffe */
        ok(s == 0xfffe, "T1 inet_cksum 0x0001 → 0xfffe");
        (void)w;
        u8 z[20]; memset(z, 0, 20);
        ok(inet_cksum(z, 20) == 0xffff, "T1 全零 20 字节 → 0xffff");
        /* 自洽：带头部校验和的 IP 头再算应为 0 */
        u8 h[20]; memset(h, 0, 20);
        h[0] = 0x45; put16(h + 2, 60); h[8] = 64; h[9] = 1;
        put32(h + 12, nas_ip); put32(h + 16, our_ip);
        put16(h + 10, inet_cksum(h, 20));
        ok(inet_cksum(h, 20) == 0, "T1 带校验和的头再算 = 0（自洽）");
        h[5] ^= 0xff;
        ok(inet_cksum(h, 20) != 0, "T1 改一个字节后校验失败（能查出坏包）");
    }

    /* T2 IP 解析/格式化 */
    {
        u32 ip = 0;
        ok(ip_parse("192.168.1.100", &ip) == 0 && ip == nas_ip, "T2 parse 192.168.1.100");
        ok(ip_parse("0.0.0.0", &ip) == 0 && ip == 0, "T2 parse 0.0.0.0");
        ok(ip_parse("255.255.255.255", &ip) == 0 && ip == 0xffffffffu, "T2 parse 255.255.255.255");
        ok(ip_parse("256.1.1.1", &ip) != 0, "T2 拒 256.x.x.x");
        ok(ip_parse("1.2.3", &ip) != 0, "T2 拒 1.2.3（缺段）");
        ok(ip_parse("1..2.3", &ip) != 0, "T2 拒空段");
        ok(ip_parse("a.b.c.d", &ip) != 0, "T2 拒字母");
        char b[16]; ip_fmt(nas_ip, b); ok(strcmp(b, "192.168.1.100") == 0, "T2 ip_fmt 回 192.168.1.100");
        ip_fmt(0, b); ok(strcmp(b, "0.0.0.0") == 0, "T2 ip_fmt 0.0.0.0");
        ip_fmt(0xffffffffu, b); ok(strcmp(b, "255.255.255.255") == 0, "T2 ip_fmt 255.255.255.255");
    }

    /* T3 UDP 校验和自洽 */
    {
        u8 u[8 + 12];
        put16(u + 0, 68); put16(u + 2, 67);
        put16(u + 4, 20);
        u[6] = u[7] = 0;
        for (int i = 0; i < 12; i++) u[8 + i] = (u8)(i * 7 + 1);
        u16 c = udp_cksum(nas_ip, our_ip, u, 20);
        ok(c != 0 && c != 0xffff, "T3 算出非平凡校验和");
        put16(u + 6, c);
        ok(udp_cksum(nas_ip, our_ip, u, 20) == 0, "T3 填回后验证 = 0");
        u[8] ^= 0x55;
        ok(udp_cksum(nas_ip, our_ip, u, 20) != 0, "T3 改载荷后验证失败");
    }

    /* 从这里起需要「已联网」状态 */
    g_up = 1; g_ip = our_ip; g_mask = mask; g_gw = gw_ip;
    vn_mac_copy(g_mac);
    g_arp[0].valid = 0; g_arp[1].valid = 0; g_arp[2].valid = 0; g_arp[3].valid = 0;
    g_arp[4].valid = 0; g_arp[5].valid = 0; g_arp[6].valid = 0; g_arp[7].valid = 0;

    /* T4 ICMP echo request → reply */
    {
        u8 f[1600];
        int n = mk_eth(f, our_mac, peer_mac, ETH_IP);
        n = mk_ip(f, n, nas_ip, our_ip, IP_ICMP, 8 + 48);
        int icl = mk_icmp_echo_request(f, n, 0x1234, 7, 48);
        n += icl;
        tx_reset();
        on_frame(f, n);
        ok(g_tx_n == 1, "T4 收到 echo request 后发出 1 帧");
        if (g_tx_n >= 1) {
            const u8 *r = g_tx[0];
            int rl = g_tx_len[0];
            ok(memcmp(r, peer_mac, 6) == 0, "T4 应答以太网目的 = 请求源 MAC");
            ok(get16(r + 12) == ETH_IP, "T4 应答类型 IPv4");
            const u8 *ip = r + 14;
            ok(ip[9] == IP_ICMP, "T4 应答 IP 协议 = ICMP");
            ok(get32(ip + 12) == our_ip, "T4 应答源 IP = 本机");
            ok(get32(ip + 16) == nas_ip, "T4 应答目的 IP = 请求方");
            ok(inet_cksum(ip, 20) == 0, "T4 应答 IP 校验和正确");
            const u8 *ic = ip + 20;
            int icl2 = rl - 14 - 20;
            ok(ic[0] == 0, "T4 应答类型 = 0（echo reply）");
            ok(get16(ic + 4) == 0x1234 && get16(ic + 6) == 7, "T4 id/seq 原样带回");
            ok(icl2 >= 8 && inet_cksum(ic, icl2) == 0, "T4 应答 ICMP 校验和正确");
            ok(icl2 == 8 + 48 && memcmp(ic + 8, "ABCDEFGH", 8) == 0, "T4 载荷原样带回");
        }
    }

    /* T5 ARP：请求 → 应答；缓存 */
    {
        u8 f[64];
        int n = mk_eth(f, bcast, peer_mac, ETH_ARP);
        u8 *a = f + n;
        put16(a + 0, 1); put16(a + 2, ETH_IP); a[4] = 6; a[5] = 4;
        put16(a + 6, ARP_REQ);
        memcpy(a + 8, peer_mac, 6);
        put32(a + 14, nas_ip);
        memset(a + 18, 0, 6);
        put32(a + 24, our_ip);
        n += 28;
        tx_reset();
        on_frame(f, n);
        ok(g_tx_n == 1, "T5 ARP 请求 → 发 1 帧应答");
        if (g_tx_n >= 1) {
            const u8 *r = g_tx[0];
            const u8 *ra = r + 14;
            ok(get16(r + 12) == ETH_ARP && get16(ra + 6) == ARP_REPLY, "T5 应答 op=2");
            ok(memcmp(ra + 8, our_mac, 6) == 0, "T5 应答 sender mac = 本机");
            ok(get32(ra + 14) == our_ip, "T5 应答 sender ip = 本机");
            ok(get32(ra + 24) == nas_ip, "T5 应答 target ip = 请求方");
        }
        u8 m[6];
        ok(arp_lookup(nas_ip, m) == 0 && memcmp(m, peer_mac, 6) == 0, "T5 请求方的 MAC 进了缓存");
        ok(arp_lookup(0x01020304u, m) != 0, "T5 未缓存的 IP 查不到");
        /* 别人的请求不应答 */
        put32(a + 24, 0x12345678u);
        tx_reset();
        on_frame(f, n);
        ok(g_tx_n == 0, "T5 问的不是本机 IP → 不应答");
    }

    /* T6 DHCP 报文解析 */
    {
        u8 dh[548];
        g_dhcp_xid = 0x4c4fbeef;
        int l = mk_dhcp(dh, g_dhcp_xid, 2, 2, our_ip, mask, gw_ip, nas_ip, our_mac);
        int mt = 0; u32 ip = 0, mk = 0, gw = 0, sv = 0;
        ok(dhcp_parse(dh, l, &mt, &ip, &mk, &gw, &sv) == 0, "T6 OFFER 可解析");
        ok(mt == 2 && ip == our_ip && mk == mask && gw == gw_ip && sv == nas_ip, "T6 OFFER 字段全对");
        l = mk_dhcp(dh, g_dhcp_xid, 2, 5, our_ip, mask, gw_ip, nas_ip, our_mac);
        ok(dhcp_parse(dh, l, &mt, &ip, &mk, &gw, &sv) == 0 && mt == 5, "T6 ACK msg_type=5");
        l = mk_dhcp(dh, g_dhcp_xid ^ 1, 2, 2, our_ip, mask, gw_ip, nas_ip, our_mac);
        ok(dhcp_parse(dh, l, &mt, &ip, &mk, &gw, &sv) != 0, "T6 xid 不符 → 拒绝");
        l = mk_dhcp(dh, g_dhcp_xid, 1, 2, our_ip, mask, gw_ip, nas_ip, our_mac);
        ok(dhcp_parse(dh, l, &mt, &ip, &mk, &gw, &sv) != 0, "T6 op=1（请求包）→ 拒绝");
        ok(dhcp_parse(dh, 100, &mt, &ip, &mk, &gw, &sv) != 0, "T6 太短 → 拒绝");
    }

    /* T7 ip_rx 过滤 */
    {
        u8 f[1600];
        st_rx_ip = 0; st_rx_drop = 0;
        int n = mk_eth(f, our_mac, peer_mac, ETH_IP);
        n = mk_ip(f, n, nas_ip, our_ip, IP_ICMP, 8);
        mk_icmp_echo_request(f, n, 1, 1, 0);
        n += 8;
        f[14 + 10] ^= 0xff;                       /* 弄坏 IP 头校验和 */
        tx_reset(); on_frame(f, n);
        ok(st_rx_ip == 0 && g_tx_n == 0, "T7 坏 IP 校验和 → 丢弃");
        /* 分片 */
        n = mk_eth(f, our_mac, peer_mac, ETH_IP);
        n = mk_ip(f, n, nas_ip, our_ip, IP_ICMP, 8);
        mk_icmp_echo_request(f, n, 1, 1, 0);
        n += 8;
        put16(f + 14 + 6, 0x2000);                /* MF 位置位 = 分片 */
        put16(f + 14 + 10, 0);
        put16(f + 14 + 10, inet_cksum(f + 14, 20));
        tx_reset(); on_frame(f, n);
        ok(st_rx_ip == 0 && st_rx_drop > 0, "T7 分片 → 丢弃");
        /* 别人的包 */
        n = mk_eth(f, our_mac, peer_mac, ETH_IP);
        n = mk_ip(f, n, nas_ip, 0x08080808u, IP_ICMP, 8);
        mk_icmp_echo_request(f, n, 1, 1, 0);
        n += 8;
        tx_reset(); on_frame(f, n);
        ok(g_tx_n == 0, "T7 目的不是本机 → 不回");
    }

    /* T8 完整 DHCP 流程（桩扮演服务器） */
    {
        g_up = 0; g_ip = g_mask = g_gw = 0; g_offered = 0; g_dhcp_server = 0;
        g_on_send = dhcp_server_stub;
        tx_reset();
        int r = net_dhcp();
        g_on_send = 0;
        ok(r == 0 && g_up && g_ip == our_ip, "T8 DHCP 全流程成功拿到 IP");
        ok(g_mask == mask && g_gw == gw_ip, "T8 掩码/网关正确");
        ok(g_tx_n == 2, "T8 只发了 DISCOVER + REQUEST 两帧（没有重试）");
        ok(strstr(g_log, "联网成功") != 0, "T8 输出里报告联网成功");
    }

    /* T9 net static */
    {
        g_up = 0; g_ip = g_mask = g_gw = 0;
        ok(net_static_config("192.168.10.77", "255.255.255.0", "192.168.10.1") == 0
           && g_ip == our_ip && g_mask == mask && g_gw == gw_ip, "T9 手配成功");
        ok(net_static_config("999.1.1.1", 0, 0) != 0, "T9 非法 IP 拒绝");
        ok(net_static_config("192.168.10.78", "255.255.0", 0) != 0, "T9 非法掩码拒绝");
    }

    printf(fails ? "== 结果：%d/%d FAIL ==\n" : "== 结果：%d/%d 全过 ==\n", total - fails, total);
    return fails ? 1 : 0;
}
