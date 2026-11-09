#include "loongos.h"   /* 统一内核接口：跨模块声明的唯一事实来源 */
/* LoongOS 阶段7 —— 最小网络栈（以太网 + ARP + IPv4 + ICMP + UDP + DHCP 客户端）
 *
 * 设计取舍（教学内核，够用为止）：
 *   - 全轮询：net_poll() 在 shell 空闲时跑，处理收包；发送是同步的（发完等 used 环）
 *   - 无分片、无重组、无 TCP —— 收到分片 IP 直接丢
 *   - ARP 缓存 8 条，不过期（虚拟机实验场景足够）
 *   - DHCP 走标准 DISCOVER→OFFER→REQUEST→ACK，拿 ip/mask/gw；失败可 net static 手配
 * 校验和按 RFC 1071 软件算（协商时没开硬件卸载）。
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;



/* ---------------- 小工具 ---------------- */
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16  get16(const u8 *p)  { return (u16)((p[0] << 8) | p[1]); }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32  get32(const u8 *p)  { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void mem_cpy(void *d, const void *s, u32 n) { u8 *dd = d; const u8 *ss = s; while (n--) *dd++ = *ss++; }
static void mem_set(void *d, u8 v, u32 n) { u8 *dd = d; while (n--) *dd++ = v; }

/* RFC1071 互联网校验和：按 16 位大端累加，折叠取反 */
static u16 inet_cksum(const u8 *data, int len)
{
    u32 sum = 0;
    int i = 0;
    for (; i + 1 < len; i += 2) sum += get16(data + i);
    if (i < len) sum += (u16)data[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

/* UDP 伪首部校验和（src ip + dst ip + 零 + 协议 + UDP 长度 + UDP 头体） */
static u16 udp_cksum(u32 src, u32 dst, const u8 *udp, int ulen)
{
    u32 sum = 0;
    sum += (src >> 16) & 0xffff; sum += src & 0xffff;
    sum += (dst >> 16) & 0xffff; sum += dst & 0xffff;
    sum += 17; sum += (u32)ulen;
    int i = 0;
    for (; i + 1 < ulen; i += 2) sum += get16(udp + i);
    if (i < ulen) sum += (u16)udp[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
    /* ⚠️ 这里不做 0→0xffff 的映射：同一函数既用于「发送时算校验和」也用于「接收时验证」。
       验证语义 = 把带校验和的整段再算一遍，合法包结果恰为 0；
       若在这映射成 0xffff，udp_rx 的 `!= 0` 判坏会把所有合法包全丢（DHCP 收不到 OFFER）。
       发送侧在 udp_send 里做 0→0xffff（UDP 里 0 表示「不校验」，必须发 0xffff）。 */
}

/* IP 字符串 <-> u32（点分十进制） */
static int ip_parse(const char *s, u32 *out)
{
    u32 v[4] = {0, 0, 0, 0};
    int part = 0, val = 0, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); digits++; if (val > 255) return -1; }
        else if (*p == '.' || *p == 0) {
            if (!digits || part >= 4) return -1;
            v[part++] = (u32)val; val = 0; digits = 0;
            if (!*p) break;
        } else return -1;
    }
    if (part != 4) return -1;          /* "1.2.3" 缺段 / "1.2.3.4.5" 多段 都要拒 */
    *out = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    return 0;
}
static void ip_fmt(u32 ip, char *buf /* 16 */)
{
    int n = 0;
    for (int s = 24; s >= 0; s -= 8) {
        u32 o = (ip >> s) & 0xff, t[3]; int m = 0;
        do { t[m++] = (int)(o % 10); o /= 10; } while (o);
        if (s != 24) buf[n++] = '.';
        while (m) buf[n++] = (char)('0' + t[--m]);
    }
    buf[n] = 0;
}

/* ---------------- 状态 ---------------- */
#define ETH_ARP  0x0806
#define ETH_IP   0x0800
#define IP_ICMP  1
#define IP_UDP   17
#define ARP_REQ  1
#define ARP_REPLY 2

static u8  g_mac[6];
static u32 g_ip, g_mask, g_gw;
static volatile unsigned g_up;        /* IP 已配置（volatile：poll_until 拿它当等待条件） */
static u32 g_dhcp_server, g_offered;

/* 统计 */
static u32 st_rx, st_tx, st_rx_arp, st_rx_ip, st_rx_icmp, st_rx_udp, st_rx_drop;

/* ARP 缓存 */
#define ARP_N 8
static struct { u32 ip; u8 mac[6]; int valid; } g_arp[ARP_N];

static int arp_lookup(u32 ip, u8 mac[6])
{
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { mem_cpy(mac, g_arp[i].mac, 6); return 0; }
    return -1;
}
static void arp_record(u32 ip, const u8 mac[6])
{
    if (ip == 0) return;
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { mem_cpy(g_arp[i].mac, mac, 6); return; }
    for (int i = 0; i < ARP_N; i++)
        if (!g_arp[i].valid) { g_arp[i].valid = 1; g_arp[i].ip = ip; mem_cpy(g_arp[i].mac, mac, 6); return; }
    /* 满了就覆盖 0 号（无 LRU，教学够用） */
    g_arp[0].valid = 1; g_arp[0].ip = ip; mem_cpy(g_arp[0].mac, mac, 6);
}

/* ---------------- 帧发送 ---------------- */
static u8 g_txframe[14 + 1500];

static int eth_send2(const u8 dst[6], u16 type, const u8 *payload, int plen)
{
    if (plen > 1500) return -1;
    mem_cpy(g_txframe, dst, 6);
    mem_cpy(g_txframe + 6, g_mac, 6);
    put16(g_txframe + 12, type);
    mem_cpy(g_txframe + 14, payload, (u32)plen);
    int total = 14 + plen;
    if (total < 60) { for (int i = total; i < 60; i++) g_txframe[i] = 0; total = 60; }  /* 以太网最小帧填充 */
    st_tx++;
    return vn_send(g_txframe, total);
}

/* 发 ARP 请求：who has <ip> */
static void arp_request(u32 ip)
{
    u8 pkt[28];
    put16(pkt + 0, 1); put16(pkt + 2, ETH_IP);
    pkt[4] = 6; pkt[5] = 4;
    put16(pkt + 6, ARP_REQ);
    mem_cpy(pkt + 8, g_mac, 6);
    put32(pkt + 14, g_up ? g_ip : 0);
    mem_set(pkt + 18, 0, 6);
    put32(pkt + 24, ip);
    static const u8 bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    eth_send2(bcast, ETH_ARP, pkt, 28);
}

/* 发 ARP 应答（回应对我 IP 的请求） */
static void arp_reply(const u8 *req)
{
    u8 pkt[28];
    put16(pkt + 0, 1); put16(pkt + 2, ETH_IP);
    pkt[4] = 6; pkt[5] = 4;
    put16(pkt + 6, ARP_REPLY);
    mem_cpy(pkt + 8, g_mac, 6);
    put32(pkt + 14, g_ip);
    mem_cpy(pkt + 18, req + 8, 6);      /* tha = 对方 mac */
    put32(pkt + 24, get32(req + 14));   /* tpa = 对方 ip */
    eth_send2(req + 8, ETH_ARP, pkt, 28);
}

/* 解析并处理一个 ARP 帧 */
static void arp_rx(const u8 *f, int len)
{
    if (len < 28) return;
    if (get16(f + 0) != 1 || get16(f + 2) != ETH_IP || f[4] != 6 || f[5] != 4) return;
    u32 spa = get32(f + 14);
    arp_record(spa, f + 8);
    st_rx_arp++;
    u16 op = get16(f + 6);
    u32 tpa = get32(f + 24);
    if (op == ARP_REQ && g_up && tpa == g_ip) arp_reply(f);
}

/* ---------------- IP/ICMP/UDP ---------------- */
/* 组一个完整 IP 包（20 字节头 + 负载）；dstmac_in 给了就直发，否则查 ARP */
static int ip_send(u32 dst_ip, u8 proto, const u8 *payload, int plen, const u8 *dstmac_in)
{
    if (plen > 1480) return -1;
    u8 pkt[20 + 1480];
    pkt[0] = 0x45; pkt[1] = 0;
    put16(pkt + 2, (u16)(20 + plen));
    put16(pkt + 4, 0); put16(pkt + 6, 0);
    pkt[8] = 64; pkt[9] = proto;
    pkt[10] = pkt[11] = 0;
    put32(pkt + 12, g_ip);
    put32(pkt + 16, dst_ip);
    put16(pkt + 10, inet_cksum(pkt, 20));
    mem_cpy(pkt + 20, payload, (u32)plen);

    u8 dstmac[6];
    if (dstmac_in) mem_cpy(dstmac, dstmac_in, 6);
    else {
        if (arp_lookup(dst_ip, dstmac) != 0) {
            /* 同网段问本人，否则问网关 */
            u32 who = ((dst_ip & g_mask) == (g_ip & g_mask)) ? dst_ip : (g_gw ? g_gw : dst_ip);
            if (arp_lookup(who, dstmac) != 0) { arp_request(who); return -2; }  /* 没缓存：本帧放弃，等 ARP 应答后再试 */
        }
    }
    return eth_send2(dstmac, ETH_IP, pkt, 20 + plen);
}

/* ping 应答的发送（供 echo 请求处理用；上面的 icmp_echo_reply 是废稿，不链接） */

/* UDP 发送（DHCP 用：src_ip/dst_ip 可为广播） */
static int udp_send(u32 src_ip, u32 dst_ip, u16 sport, u16 dport, const u8 *payload, int plen)
{
    u8 udp[20 + 1480];
    put16(udp + 0, sport); put16(udp + 2, dport);
    put16(udp + 4, (u16)(8 + plen));
    udp[6] = udp[7] = 0;
    mem_cpy(udp + 8, payload, (u32)plen);
    int ulen = 8 + plen;
    {   /* 发送侧：算出 0 也要发 0xffff（UDP 里 0 = 不校验） */
        u16 c = udp_cksum(src_ip, dst_ip, udp, ulen);
        put16(udp + 6, c ? c : 0xffff);
    }

    if (src_ip == 0 || dst_ip == 0xffffffffu) {
        /* 广播：直接拼以太网层（不走 ip_send 的 ARP） */
        u8 pkt[20 + 1480];
        pkt[0] = 0x45; pkt[1] = 0;
        put16(pkt + 2, (u16)(20 + ulen));
        put16(pkt + 4, 0); put16(pkt + 6, 0);
        pkt[8] = 64; pkt[9] = IP_UDP;
        pkt[10] = pkt[11] = 0;
        put32(pkt + 12, src_ip);
        put32(pkt + 16, dst_ip);
        put16(pkt + 10, inet_cksum(pkt, 20));
        mem_cpy(pkt + 20, udp, (u32)ulen);
        static const u8 bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
        return eth_send2(bcast, ETH_IP, pkt, 20 + ulen);
    }
    return ip_send(dst_ip, IP_UDP, udp, ulen, 0);
}

/* ---------------- 收包总入口 ---------------- */
/* ping 等待用的匹配状态 */
static volatile unsigned g_ping_got;
static u16 g_ping_id, g_ping_seq;

/* 收 ICMP。srcmac = 发来这个 IP 包的以太网源 MAC（回包直接用它，别再翻 IP 头里的字节）
 * ⚠️ 之前这里从 ip_pkt+6 取目的 MAC —— 那是 IP 头里的 flags/TTL/checksum，取出来是垃圾，
 *    真机上 ping 回复会发往不存在的 MAC（NAS ping VM 永远不通）。主机测试抓到的。 */
static void icmp_rx(const u8 *ip_pkt, int iplen, const u8 *srcmac)
{
    int ihl = (ip_pkt[0] & 0x0f) * 4;
    if (ihl < 20 || iplen < ihl + 8) return;
    const u8 *ic = ip_pkt + ihl;
    int iclen = iplen - ihl;
    if (inet_cksum(ic, iclen) != 0) { st_rx_drop++; return; }   /* 坏包丢弃 */
    st_rx_icmp++;
    u8 type = ic[0];
    int olen = iclen;
    if (type == 8) {                                   /* echo request → 回复 */
        static u8 out[1500];
        mem_cpy(out, ic, (u32)olen);
        out[0] = 0; out[2] = out[3] = 0;
        put16(out + 2, inet_cksum(out, olen));
        ip_send(get32(ip_pkt + 12), IP_ICMP, out, olen, srcmac);
    } else if (type == 0) {                            /* echo reply → 交给 ping */
        if (olen >= 8 && get16(ic + 4) == g_ping_id && get16(ic + 6) == g_ping_seq)
            g_ping_got = 1;
    }
}

/* DHCP 报文构造/解析 */
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

static u32 g_dhcp_xid;

static int dhcp_send_discover(void)
{
    u8 p[548];
    mem_set(p, 0, sizeof p);
    p[0] = 1; p[1] = 1; p[2] = 6;      /* op=bootrequest htype=eth hlen=6 */
    put32(p + 4, g_dhcp_xid);
    mem_cpy(p + 28, g_mac, 6);         /* chaddr */
    put32(p + 236, 0x63825363);        /* magic cookie */
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = 1;                  /* DISCOVER */
    p[o++] = 55; p[o++] = 4; p[o++] = 1; p[o++] = 3; p[o++] = 6; p[o++] = 15;  /* 要 mask/gw/dns */
    p[o++] = 255;
    return udp_send(0, 0xffffffffu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, p, o);
}

static int dhcp_send_request(u32 req_ip, u32 server)
{
    u8 p[548];
    mem_set(p, 0, sizeof p);
    p[0] = 1; p[1] = 1; p[2] = 6;
    put32(p + 4, g_dhcp_xid);
    mem_cpy(p + 28, g_mac, 6);
    put32(p + 236, 0x63825363);
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = 3;                  /* REQUEST */
    p[o++] = 50; p[o++] = 4; put32(p + o, req_ip); o += 4;  /* requested ip */
    p[o++] = 54; p[o++] = 4; put32(p + o, server); o += 4;  /* server id */
    p[o++] = 255;
    return udp_send(0, 0xffffffffu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, p, o);
}

/* 解析 DHCP 应答（OFFER/ACK）；msg_type 回传 2=OFFER 5=ACK */
static int dhcp_parse(const u8 *udp_payload, int len, int *msg_type, u32 *ip, u32 *mask, u32 *gw, u32 *server)
{
    if (len < 240) return -1;
    if (udp_payload[0] != 2) return -1;                   /* op=bootreply */
    if (get32(udp_payload + 236) != 0x63825363) return -1;
    if (get32(udp_payload + 4) != g_dhcp_xid) return -1;  /* xid 对不上：不是给我们的 */
    *ip = get32(udp_payload + 16);                        /* yiaddr */
    *mask = 0; *gw = 0; *server = 0; *msg_type = 0;
    int i = 240;
    while (i < len) {
        u8 code = udp_payload[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }
        if (i + 1 >= len) break;
        int l = udp_payload[i + 1];
        if (i + 2 + l > len) break;
        const u8 *v = udp_payload + i + 2;
        if (code == 53 && l >= 1) *msg_type = v[0];
        else if (code == 1 && l >= 4) *mask = get32(v);
        else if (code == 3 && l >= 4) *gw = get32(v);
        else if (code == 54 && l >= 4) *server = get32(v);
        i += 2 + l;
    }
    return 0;
}

/* 收到 UDP：分流 DHCP（68 端口） */
static void udp_rx(const u8 *ip_pkt, int iplen)
{
    int ihl = (ip_pkt[0] & 0x0f) * 4;
    if (iplen < ihl + 8) return;
    const u8 *u = ip_pkt + ihl;
    int ulen = iplen - ihl;
    u16 dport = get16(u + 2);
    st_rx_udp++;
    if (dport != DHCP_CLIENT_PORT) return;
    /* 校验和可选：算出来不为 0 且不等于 0xffff 才算坏 */
    if (get16(u + 6) != 0) {
        u32 src = get32(ip_pkt + 12), dst = get32(ip_pkt + 16);
        if (udp_cksum(src, dst, u, ulen) != 0) { st_rx_drop++; return; }
    }
    int mt = 0; u32 yip = 0, mask = 0, gw = 0, srv = 0;
    if (dhcp_parse(u + 8, ulen - 8, &mt, &yip, &mask, &gw, &srv) == 0) {
        if (mt == 2) {                     /* OFFER */
            g_offered = yip;
            g_dhcp_server = srv;
        } else if (mt == 5) {              /* ACK */
            g_ip = yip; g_mask = mask ? mask : 0xffffff00u; g_gw = gw;
            g_dhcp_server = srv;
            g_up = 1;
        }
    }
}

/* 给 tcp.c 用：发一个已组好的 TCP 段（IP 头由 ip_send 补） */
int ip_send_tcp(u32 dst_ip, const u8 *seg, int len)
{
    return ip_send(dst_ip, 6 /* TCP */, seg, len, 0);
}
u32 net_my_ip(void) { return g_ip; }

static void ip_rx(const u8 *f, int len, const u8 *srcmac)
{
    if (len < 20) { st_rx_drop++; return; }
    if ((f[0] >> 4) != 4) { st_rx_drop++; return; }
    int ihl = (f[0] & 0x0f) * 4;
    if (ihl < 20 || len < ihl) { st_rx_drop++; return; }
    int tot = get16(f + 2);
    if (tot > len || tot < ihl) { st_rx_drop++; return; }
    if (get16(f + 6) & 0x3fff) { st_rx_drop++; return; }   /* 分片：不支持，丢 */
    if (inet_cksum(f, ihl) != 0) { st_rx_drop++; return; }
    u32 dst = get32(f + 16);
    /* 只收给我们的、广播或（未配 IP 时的 DHCP 应答走广播地址） */
    if (!(dst == g_ip || dst == 0xffffffffu || (!g_up && dst == 0))) { st_rx_drop++; return; }
    st_rx_ip++;
    u8 proto = f[9];
    if (proto == IP_ICMP) icmp_rx(f, tot, srcmac);
    else if (proto == IP_UDP) udp_rx(f, tot);
    else if (proto == 6) { extern void tcp_rx(const u8 *, int); tcp_rx(f, tot); }
    else st_rx_drop++;
}

static int on_frame(const u8 *frame, int len)
{
    st_rx++;
    if (len < 14) { st_rx_drop++; return 0; }
    u16 type = get16(frame + 12);
    if (type == ETH_ARP)       arp_rx(frame + 14, len - 14);
    else if (type == ETH_IP)   ip_rx(frame + 14, len - 14, frame + 6);
    else st_rx_drop++;
    return 0;
}

/* shell 空闲时调用：能收多少收多少 */
void net_poll(void)
{
    if (vn_present()) vn_poll(on_frame);
    { extern void tcp_tick(void); tcp_tick(); }
}

/* ---------------- 对外命令 ---------------- */
/* ---------------- 时间源 ----------------
 * 内核里直接读通用定时器（cntpct_el0 / cntfrq_el0）。
 * 主机端单元测试（test_net_host.c）会定义 NET_NO_CNT —— Linux 用户态读 cntpct_el0
 * 会被内核禁掉直接 SIGILL（实测 Illegal instruction），所以测试里注入假时钟。 */
#ifndef NET_NO_CNT
static inline u64 cnt_read(void) { u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline u64 cnt_freq(void) { u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#else
extern u64 cnt_read(void);
extern u64 cnt_freq(void);
#endif

/* 轮询等待条件成立，timeout_ms 毫秒内；期间持续收包 */
static int poll_until(volatile unsigned *flag, int timeout_ms)
{
    u64 freq = cnt_freq();
    u64 deadline = cnt_read() + freq * (u64)timeout_ms / 1000ull;
    while (!*flag) {
        net_poll();
        if (cnt_read() >= deadline) return -1;
    }
    return 0;
}

int net_init_driver(void)
{
    if (vn_present()) { kputs("[net ] 驱动已初始化\n"); return 0; }
    int r = vn_init();
    if (r != 0) {
        kputs("[net ] virtio-net 初始化失败 r="); kputdec((u32)(-r)); kputs("\n");
        return r;
    }
    vn_mac_copy(g_mac);
    kputs("[net ] MAC ");
    for (int i = 0; i < 6; i++) {
        if (i) kputs(":");
        char hx[3]; hx[0] = "0123456789abcdef"[g_mac[i] >> 4]; hx[1] = "0123456789abcdef"[g_mac[i] & 15]; hx[2] = 0;
        kputs(hx);
    }
    kputs(vn_link() ? "  链路 ✅ up\n" : "  链路 ⚠️ down\n");
    return 0;
}

int net_dhcp(void)
{
    if (!vn_present()) { kputs("[net ] 驱动不在，先 net\n"); return -1; }
    g_dhcp_xid = 0x4c4f0000u | (u32)(tick & 0xffff);
    kputs("[net ] DHCP: 发 DISCOVER …\n");
    for (int try = 0; try < 3 && !g_offered; try++) {
        dhcp_send_discover();
        poll_until(&g_offered, 2000);
    }
    if (!g_offered) { kputs("[net ] DHCP: 没收到 OFFER（试试 net static <ip> <掩码> <网关>）\n"); return -2; }
    char b[16];
    ip_fmt(g_offered, b); kputs("[net ] OFFER: "); kputs(b);
    if (g_dhcp_server) { ip_fmt(g_dhcp_server, b); kputs("  服务器 "); kputs(b); }
    kputs("\n[net ] 发 REQUEST …\n");
    /* ⚠️ 等的是 g_up（ACK 到达时 udp_rx 会置 1），不是本地 ack —— 原来等 &ack 每次白等满 2 秒超时 */
    for (int try = 0; try < 3 && !g_up; try++) {
        dhcp_send_request(g_offered, g_dhcp_server);
        poll_until(&g_up, 2000);
    }
    if (!g_up) { kputs("[net ] DHCP: 没收到 ACK\n"); return -3; }
    ip_fmt(g_ip, b);  kputs("[net ] IP   "); kputs(b); kputs("\n");
    ip_fmt(g_mask, b); kputs("[net ] 掩码 "); kputs(b); kputs("\n");
    if (g_gw) { ip_fmt(g_gw, b); kputs("[net ] 网关 "); kputs(b); kputs("\n"); }
    kputs("[net ] 联网成功 ✅\n");
    return 0;
}

int net_static_config(const char *ip_s, const char *mask_s, const char *gw_s)
{
    if (ip_parse(ip_s, &g_ip) != 0) { kputs("[net ] IP 格式不对\n"); return -1; }
    if (mask_s && *mask_s) { if (ip_parse(mask_s, &g_mask) != 0) { kputs("[net ] 掩码格式不对\n"); return -1; } }
    else g_mask = 0xffffff00u;
    if (gw_s && *gw_s) { if (ip_parse(gw_s, &g_gw) != 0) { kputs("[net ] 网关格式不对\n"); return -1; } }
    else g_gw = 0;
    g_up = 1;
    char b[16];
    ip_fmt(g_ip, b); kputs("[net ] 手配 IP "); kputs(b);
    ip_fmt(g_mask, b); kputs(" / "); kputs(b);
    if (g_gw) { ip_fmt(g_gw, b); kputs("  网关 "); kputs(b); }
    kputs("  ✅\n");
    return 0;
}

void net_ifconfig_print(void)
{
    char b[16];
    if (!vn_present()) { kputs("网卡: 无（先 net 初始化驱动）\n"); return; }
    kputs("网卡: virtio-net  MAC ");
    for (int i = 0; i < 6; i++) {
        if (i) kputs(":");
        char hx[3]; hx[0] = "0123456789abcdef"[g_mac[i] >> 4]; hx[1] = "0123456789abcdef"[g_mac[i] & 15]; hx[2] = 0;
        kputs(hx);
    }
    kputs(vn_link() ? "  link up\n" : "  link down\n");
    if (!g_up) { kputs("IP: 未配置（net 走 DHCP，或 net static <ip> <掩码> <网关>）\n"); }
    else {
        ip_fmt(g_ip, b); kputs("IP: "); kputs(b);
        ip_fmt(g_mask, b); kputs("  掩码 "); kputs(b);
        if (g_gw) { ip_fmt(g_gw, b); kputs("  网关 "); kputs(b); }
        kputs("\n");
    }
    kputs("统计: 收="); kputdec(st_rx); kputs(" 发="); kputdec(st_tx);
    kputs(" arp="); kputdec(st_rx_arp); kputs(" ip="); kputdec(st_rx_ip);
    kputs(" icmp="); kputdec(st_rx_icmp); kputs(" udp="); kputdec(st_rx_udp);
    kputs(" 丢="); kputdec(st_rx_drop); kputs("\n");
    kputs("ARP 缓存:\n");
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].valid) {
            ip_fmt(g_arp[i].ip, b); kputs("  "); kputs(b); kputs(" -> ");
            for (int j = 0; j < 6; j++) {
                if (j) kputs(":");
                char hx[3]; hx[0] = "0123456789abcdef"[g_arp[i].mac[j] >> 4]; hx[1] = "0123456789abcdef"[g_arp[i].mac[j] & 15]; hx[2] = 0;
                kputs(hx);
            }
            kputs("\n");
        }
}

/* ping <ip> [次数]：ICMP echo，带 RTT */
int net_ping(const char *ip_s, int count)
{
    if (!g_up) { kputs("[net ] 先配 IP（net / net static）\n"); return -1; }
    u32 dst;
    if (ip_parse(ip_s, &dst) != 0) { kputs("[net ] 目标 IP 格式不对\n"); return -1; }
    if (count <= 0 || count > 10) count = 4;

    /* 先把网关/目标的 MAC 解析出来（同网段问目标，否则问网关） */
    u32 who = ((dst & g_mask) == (g_ip & g_mask)) ? dst : (g_gw ? g_gw : dst);
    u8 mac[6];
    u64 freq = cnt_freq();
    for (int try = 0; try < 5 && arp_lookup(who, mac) != 0; try++) {
        arp_request(who);
        u64 dl = cnt_read() + freq / 5;
        while (cnt_read() < dl) net_poll();
    }
    if (arp_lookup(who, mac) != 0) { kputs("[net ] ARP 解析失败（没人应答）\n"); return -2; }

    g_ping_id = 0x4c4f;                 /* "LO" */
    int ok = 0;
    char b[16];
    ip_fmt(dst, b);
    for (int i = 0; i < count; i++) {
        static u8 echo[64];
        int elen = 48;
        echo[0] = 8; echo[1] = 0; echo[2] = echo[3] = 0;
        put16(echo + 4, g_ping_id);
        g_ping_seq = (u16)(i + 1);
        put16(echo + 6, g_ping_seq);
        for (int j = 8; j < elen; j++) echo[j] = (u8)('a' + (j % 26));
        put16(echo + 2, inet_cksum(echo, elen));
        g_ping_got = 0;
        u64 t0 = cnt_read();
        ip_send(dst, IP_ICMP, echo, elen, mac);
        if (poll_until(&g_ping_got, 1000) == 0) {
            u32 us = (u32)((cnt_read() - t0) * 1000000ull / freq);
            kputs("来自 "); kputs(b); kputs(" 的回复: seq=");
            kputdec((u32)g_ping_seq);
            kputs(" 时间="); kputdec(us / 1000); kputdec(us % 1000); kputs(" ms\n");
            ok++;
        } else {
            kputs("seq="); kputdec((u32)g_ping_seq); kputs(" 超时\n");
        }
    }
    kputs("完成："); kputdec((u32)ok); kputs("/"); kputdec((u32)count); kputs(" 成功\n");
    return ok;
}
