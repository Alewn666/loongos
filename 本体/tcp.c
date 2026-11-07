/* LoongOS 阶段8 —— 最小 TCP（RFC 793 子集）
 *
 * 目标：能连出去、能收发、能体面关闭。够 SSH 握手 / HTTP GET 用。
 * 刻意不做（教学内核，先能跑）：
 *   - 窗口缩放/选择确认/时间戳/TFO
 *   - 拥塞控制（cwnd 固定，发一个等一个 ACK，天然不会打爆对端）
 *   - 乱序重组（收到的段必须按序，否则丢弃重发，靠对端重传兜底）
 *   - 服务端被动打开里完整的 SYN 队列（只支持 1 条连接的 LISTEN，够 sshd 最小化用）
 *
 * 状态机（客户端为主）：
 *   CLOSED --send SYN--> SYN_SENT --recv SYN|ACK + send ACK--> ESTABLISHED
 *   ESTABLISHED --send FIN--> FIN_WAIT_1 --recv ACK--> FIN_WAIT_2 --recv FIN + send ACK--> TIME_WAIT
 *   ESTABLISHED --recv FIN--> CLOSE_WAIT --close: send FIN--> LAST_ACK --recv ACK--> CLOSED
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

extern void kputs(const char *s);
extern void kputhex(u64 v);
extern void kputdec(u32 v);
extern volatile u64 tick;

/* net.c 提供的收发原语 */
extern int  ip_send_tcp(u32 dst_ip, const u8 *seg, int len);

/* 时钟（同 net.c：测试可注入） */
#ifndef NET_NO_CNT
static inline u64 tcp_cnt(void) { u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline u64 tcp_freq(void) { u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#else
extern u64 cnt_read(void);
extern u64 cnt_freq(void);
static inline u64 tcp_cnt(void) { return cnt_read(); }
static inline u64 tcp_freq(void) { return cnt_freq(); }
#endif

#define IPPROTO_TCP_ 6

/* ---- 状态 ---- */
#define TS_CLOSED      0
#define TS_SYN_SENT    1
#define TS_SYN_RCVD    2
#define TS_ESTAB       3
#define TS_FIN_WAIT1   4
#define TS_FIN_WAIT2   5
#define TS_CLOSE_WAIT  6
#define TS_CLOSING     7
#define TS_LAST_ACK    8
#define TS_TIME_WAIT   9
#define TS_LISTEN     10

#define TCP_RXBUF 4096
#define TCP_TXBUF 1536            /* 一次最多带 1460 字节载荷（单段在飞） */
#define TCP_MSS   1460
#define TCP_RETRIES 6
#define TCP_RTO_MS_BASE 250
#define TCP_TW_MS 2000            /* TIME_WAIT 停留（教学缩到 2 秒） */

static int  t_state = TS_CLOSED;
static u32  t_rip, t_myip;
static u16  t_rport, t_lport;
static u32  t_iss, t_snd_una, t_snd_nxt, t_rcv_nxt;
static u16  t_peer_wnd;
static u8   t_rx[TCP_RXBUF];
static int  t_rx_head, t_rx_len;          /* 应用层读取游标 */
static u8   t_txseg[TCP_TXBUF];           /* 最近一次未确认段（重传用） */
static int  t_txlen;
static int  t_retries;
static u64  t_timer_start;                /* 0 = 未计时 */
static u64  t_tw_start;
static u32  t_seq_synack;                 /* 被动打开时对方期望我们确认的值 */
static u16  t_listen_port;

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16  get16(const u8 *p)  { return (u16)((p[0] << 8) | p[1]); }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32  get32(const u8 *p)  { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void mcpy(void *d, const void *s, u32 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
typedef int s32;
static int  seq_lt(u32 a, u32 b) { return (s32)(a - b) < 0; }
static int  seq_le(u32 a, u32 b) { return (s32)(a - b) <= 0; }

/* 伪首部校验和（src/dst/0/6/长度） */
static u16 tcp_cksum(u32 src, u32 dst, const u8 *seg, int len)
{
    u32 s = 0;
    s += (src >> 16) & 0xffff; s += src & 0xffff;
    s += (dst >> 16) & 0xffff; s += dst & 0xffff;
    s += IPPROTO_TCP_; s += (u32)len;
    int i = 0;
    for (; i + 1 < len; i += 2) s += ((u32)seg[i] << 8) | seg[i + 1];
    if (i < len) s += (u32)seg[i] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (u16)~s;
}

/* 组并发送一个 TCP 段（flags 用 TCP_F_*） */
#define TCP_F_FIN 0x01
#define TCP_F_SYN 0x02
#define TCP_F_RST 0x04
#define TCP_F_PSH 0x08
#define TCP_F_ACK 0x10

static int send_seg(u32 seq, u32 ack, u8 flags, const u8 *data, int dlen, u32 dstip, u16 dport, u16 sport, u32 myip)
{
    static u8 seg[20 + TCP_MSS];
    if (dlen < 0 || dlen > TCP_MSS) return -1;
    put16(seg + 0, sport);
    put16(seg + 2, dport);
    put32(seg + 4, seq);
    put32(seg + 8, ack);
    seg[12] = 5 << 4;                  /* 数据偏移 20 字节，无选项 */
    seg[13] = flags;
    put16(seg + 14, TCP_RXBUF > 65535 ? 65535 : (u16)(TCP_RXBUF - t_rx_len));  /* 通告窗口 */
    put16(seg + 16, 0);                /* 校验和 */
    put16(seg + 18, 0);                /* 紧急指针 */
    if (dlen) mcpy(seg + 20, data, (u32)dlen);
    put16(seg + 16, tcp_cksum(myip, dstip, seg, 20 + dlen));
    return ip_send_tcp(dstip, seg, 20 + dlen);
}

/* 应用层可读字节数 */
int tcp_available(void) { return t_rx_len; }
int tcp_state(void) { return t_state; }
int tcp_established(void) { return t_state == TS_ESTAB; }

/* 把冲突/异常连接复位 */
static void tcp_reset_state(void)
{
    t_state = TS_CLOSED;
    t_rx_head = t_rx_len = 0;
    t_txlen = 0;
    t_timer_start = 0;
    t_retries = 0;
}

/* 主动连接；timeout_ms 内没建起来就失败 */
int tcp_connect(u32 ip, u16 port, int timeout_ms)
{
    extern u32 net_my_ip(void);
    t_myip = net_my_ip();
    if (!t_myip) return -3;
    t_rip = ip; t_rport = port;
    t_lport = (u16)(40000 + (tcp_cnt() % 20000));
    t_iss = (u32)(tcp_cnt() ^ (tcp_cnt() >> 17));
    t_snd_una = t_snd_nxt = t_iss;
    t_rcv_nxt = 0;
    t_rx_head = t_rx_len = 0;
    t_txlen = 0;
    t_retries = 0;
    t_state = TS_SYN_SENT;

    u64 freq = tcp_freq();
    u64 deadline = tcp_cnt() + freq * (u64)timeout_ms / 1000ull;
    int sent = 0;
    u64 last_send = 0;
    while (t_state == TS_SYN_SENT) {
        if (!sent || (tcp_cnt() - last_send) > freq * (TCP_RTO_MS_BASE * 2) / 1000ull) {
            send_seg(t_iss, 0, TCP_F_SYN, 0, 0, t_rip, t_rport, t_lport, t_myip);
            last_send = tcp_cnt();
            if (++t_retries > TCP_RETRIES) { tcp_reset_state(); return -1; }
            sent = 1;
        }
        extern void net_poll(void);
        net_poll();
        if (t_state != TS_SYN_SENT && t_state != TS_ESTAB) { tcp_reset_state(); return -2; }
        if (tcp_cnt() > deadline) { tcp_reset_state(); return -4; }
    }
    return t_state == TS_ESTAB ? 0 : -1;
}

/* 发送一段（同步等 ACK；超时重传） */
int tcp_send(const void *buf, int len)
{
    if (t_state != TS_ESTAB) return -1;
    if (len <= 0) return 0;
    if (len > TCP_MSS) len = TCP_MSS;
    mcpy(t_txseg, buf, (u32)len);
    t_txlen = len;
    u32 base = t_snd_nxt;
    u64 freq = tcp_freq();
    t_retries = 0;
    for (;;) {
        send_seg(base, t_rcv_nxt, TCP_F_ACK | TCP_F_PSH, t_txseg, t_txlen,
                 t_rip, t_rport, t_lport, t_myip);
        u64 dl = tcp_cnt() + freq * TCP_RTO_MS_BASE / 1000ull;
        while (tcp_cnt() < dl) {
            extern void net_poll(void);
            net_poll();
            if (t_snd_una == base + (u32)t_txlen) { t_txlen = 0; return len; }   /* 全确认 */
            if (t_state != TS_ESTAB) { return -1; }
        }
        if (++t_retries > TCP_RETRIES) { t_txlen = 0; tcp_reset_state(); return -2; }
    }
}

/* 接收：等到至少 1 字节，或超时返回 0，出错 -1 */
int tcp_recv(void *buf, int max, int timeout_ms)
{
    u64 freq = tcp_freq();
    u64 deadline = tcp_cnt() + freq * (u64)timeout_ms / 1000ull;
    while (t_rx_len == 0) {
        if (t_state == TS_CLOSED) return -1;
        extern void net_poll(void);
        net_poll();
        if (t_rx_len == 0 && tcp_cnt() > deadline) return 0;
    }
    int n = t_rx_len < max ? t_rx_len : max;
    mcpy(buf, t_rx + t_rx_head, (u32)n);
    t_rx_head += n;
    t_rx_len -= n;
    if (t_rx_len == 0) t_rx_head = 0;
    return n;
}

/* 礼貌关闭（发 FIN，等 ACK 与对方 FIN） */
void tcp_close(void)
{
    if (t_state == TS_ESTAB || t_state == TS_CLOSE_WAIT) {
        u32 seq = t_snd_nxt;
        send_seg(seq, t_rcv_nxt, TCP_F_FIN | TCP_F_ACK, 0, 0, t_rip, t_rport, t_lport, t_myip);
        t_state = (t_state == TS_CLOSE_WAIT) ? TS_LAST_ACK : TS_FIN_WAIT1;
        u64 freq = tcp_freq();
        u64 dl = tcp_cnt() + freq / 2;
        while (tcp_cnt() < dl) {
            extern void net_poll(void);
            net_poll();
            if (t_state == TS_CLOSED || t_state == TS_TIME_WAIT) break;
        }
    }
    tcp_reset_state();
}

/* ---- 收包 ---- */
void tcp_rx(const u8 *ip, int iplen)
{
    if (iplen < 20) return;
    int ihl = (ip[0] & 0x0f) * 4;
    if (iplen < ihl + 20) return;
    const u8 *seg = ip + ihl;
    int slen = iplen - ihl;
    u16 sport = get16(seg), dport = get16(seg + 2);
    u32 seq = get32(seg + 4), ack = get32(seg + 8);
    int doff = (seg[12] >> 4) * 4;
    u8 flags = seg[13];
    if (doff < 20 || slen < doff) return;
    /* 校验和验证（含伪首部） */
    if (tcp_cksum(get32(ip + 12), get32(ip + 16), seg, slen) != 0) return;
    int dlen = slen - doff;
    const u8 *data = seg + doff;
    u16 wnd = get16(seg + 14);

    /* 复位 */
    if (flags & TCP_F_RST) {
        if (t_state != TS_CLOSED && sport == t_rport) tcp_reset_state();
        return;
    }

    /* 被动打开：有人连我们监听的端口 */
    if (t_state == TS_LISTEN && dport == t_listen_port && (flags & TCP_F_SYN)) {
        t_rip = get32(ip + 12); t_rport = sport;
        t_myip = get32(ip + 16);
        t_lport = t_listen_port;
        t_iss = (u32)(tcp_cnt() ^ (tcp_cnt() >> 19));
        t_rcv_nxt = seq + 1;
        t_snd_una = t_snd_nxt = t_iss;
        t_seq_synack = seq;
        t_rx_head = t_rx_len = 0;
        send_seg(t_iss, t_rcv_nxt, TCP_F_SYN | TCP_F_ACK, 0, 0, t_rip, t_rport, t_lport, t_myip);
        t_snd_nxt = t_iss + 1;
        t_state = TS_SYN_RCVD;
        return;
    }
    /* 不是给当前连接的段 */
    if (t_state == TS_CLOSED || t_state == TS_LISTEN) return;
    if (sport != t_rport || dport != t_lport) return;
    t_peer_wnd = wnd;

    switch (t_state) {
    case TS_SYN_SENT:
        if ((flags & (TCP_F_SYN | TCP_F_ACK)) == (TCP_F_SYN | TCP_F_ACK) && ack == t_iss + 1) {
            t_rcv_nxt = seq + 1;
            t_snd_una = t_snd_nxt = t_iss + 1;
            send_seg(t_snd_nxt, t_rcv_nxt, TCP_F_ACK, 0, 0, t_rip, t_rport, t_lport, t_myip);
            t_state = TS_ESTAB;
            return;
        }
        if (flags & TCP_F_RST) { tcp_reset_state(); return; }
        return;
    case TS_SYN_RCVD:
        if ((flags & TCP_F_ACK) && ack == t_iss + 1) { t_state = TS_ESTAB; }
        else return;
        break;                                   /* 继续处理可能捎带的数据 */
    default:
        break;
    }

    /* ACK 处理：推进 snd_una */
    if (flags & TCP_F_ACK) {
        if (seq_le(t_snd_una, ack) && seq_le(ack, t_snd_nxt)) t_snd_una = ack;
        if (t_state == TS_FIN_WAIT1) {
            if (ack == t_snd_nxt) t_state = TS_FIN_WAIT2;
        } else if (t_state == TS_LAST_ACK) {
            if (ack == t_snd_nxt) { tcp_reset_state(); return; }
        } else if (t_state == TS_CLOSING) {
            if (ack == t_snd_nxt) t_state = TS_TIME_WAIT, t_tw_start = tick;
        }
    }

    /* 数据：严格按序收（乱序丢弃，靠对端重传） */
    if (dlen > 0) {
        if (seq == t_rcv_nxt) {
            int space = TCP_RXBUF - t_rx_len;
            int n = dlen < space ? dlen : space;
            if (n > 0) {
                mcpy(t_rx + t_rx_len, data, (u32)n);
                t_rx_len += n;
                t_rcv_nxt += (u32)n;
            }
            /* 空间不够就把窗口吃完的部分丢掉，避免死锁：仍确认已收部分 */
            send_seg(t_snd_nxt, t_rcv_nxt, TCP_F_ACK, 0, 0, t_rip, t_rport, t_lport, t_myip);
        } else if (seq_lt(seq, t_rcv_nxt)) {
            /* 重复段：直接 ACK 当前期望值，让对端别傻等 */
            send_seg(t_snd_nxt, t_rcv_nxt, TCP_F_ACK, 0, 0, t_rip, t_rport, t_lport, t_myip);
        } else {
            send_seg(t_snd_nxt, t_rcv_nxt, TCP_F_ACK, 0, 0, t_rip, t_rport, t_lport, t_myip);
        }
    }

    /* FIN */
    if (flags & TCP_F_FIN) {
        if (seq + (u32)dlen == t_rcv_nxt) {
            t_rcv_nxt += 1;
            send_seg(t_snd_nxt, t_rcv_nxt, TCP_F_ACK, 0, 0, t_rip, t_rport, t_lport, t_myip);
            if (t_state == TS_ESTAB) {
                t_state = TS_CLOSE_WAIT;
            } else if (t_state == TS_FIN_WAIT2) {
                t_state = TS_TIME_WAIT;
                t_tw_start = tick;
            } else if (t_state == TS_FIN_WAIT1) {
                t_state = TS_CLOSING;
            }
        }
    }
}

/* TIME_WAIT 超时回收（由 net_poll 调用） */
void tcp_tick(void)
{
    if (t_state == TS_TIME_WAIT && (tick - t_tw_start) * 100ull > TCP_TW_MS) {
        tcp_reset_state();
    }
}

/* 监听（最小实现：单连接） */
int tcp_listen(u16 port)
{
    extern u32 net_my_ip(void);
    t_myip = net_my_ip();
    if (!t_myip) return -1;
    t_listen_port = port;
    t_state = TS_LISTEN;
    return 0;
}
