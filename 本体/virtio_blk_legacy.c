/* LoongOS 阶段6③ —— virtio-blk 的 **legacy** 实现（modern 失败时的回退路径）
 *
 * 背景（2026-10-25）：modern 路径在 QEMU 的 transitional 设备上读设备特征失败
 * （device_feature_select 写不生效，feat_lo 与 feat_hi 读到同一个值 → 判定"不支持 VIRTIO 1.0"），
 * 而 legacy 路径同设备实测可用（写读校验通过）。所以两套都留，init 时先现代后传统。
 *
 * legacy 要点：
 *   寄存器在 I/O BAR（QEMU virt 把它映射到 0x3eff0000 起的 I/O 窗口，可当普通内存访问）
 *   队列用「连续内存 + QUEUE_PFN」：desc/avail 在前 4KB，used 在下一个 4KB 边界
 *   ⚠️ MMIO 一律走内联汇编单次 ldr/str（后索引寻址会触发 KVM 的 NISV 路径，见第十四轮）
 */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define L_PCI_ECAM_BASE   0x4010000000ULL
#define L_PCI_IO_WINDOW   0x3eff0000UL

#define L_PCI_VENDOR      0x00
#define L_PCI_CMD         0x04
#define L_PCI_BAR0        0x10
#define L_PCI_HEADER      0x0E
#define L_PCI_CMD_IOSE    (1u << 0)
#define L_PCI_CMD_MSE     (1u << 1)
#define L_PCI_CMD_BME     (1u << 2)

#define L_VENDOR          0x1AF4
#define L_BLK_TRANS       0x1001

#define LQ_HOST_FEAT      0x00
#define LQ_GUEST_FEAT     0x04
#define LQ_QUEUE_PFN      0x08
#define LQ_QUEUE_NUM      0x0c
#define LQ_QUEUE_SEL      0x0e
#define LQ_QUEUE_NOTIFY   0x10
#define LQ_STATUS         0x12
#define LQ_ISR            0x13
#define LQ_CONFIG         0x14

#define LST_ACK           1
#define LST_DRIVER        2
#define LST_DRIVER_OK     4
#define LST_FAILED        0x80

#define LT_IN             0
#define LT_OUT            1

#define LVDESC_NEXT       (1u << 0)
#define LVDESC_WRITE      (1u << 1)

struct ldesc { u64 addr; u32 len; u16 flags; u16 next; };
struct lhdr   { u32 type; u32 rsv; u64 sector; };

static inline u8  r8(u64 a)  { u8 v;  __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u16 r16(u64 a) { u16 v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline u32 r32(u64 a) { u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory"); return v; }
static inline void w8(u64 a, u8 v)   { __asm__ volatile("strb %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void w16(u64 a, u16 v) { __asm__ volatile("strh %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void w32(u64 a, u32 v) { __asm__ volatile("str %w1, [%0]" :: "r"(a), "r"((u32)v) : "memory"); }
static inline void lbar(void)        { __asm__ volatile("dsb sy" ::: "memory"); }

static u64  l_io;
static u64  l_cap;
static u16  l_qnum;
static u8   l_ring[8192] __attribute__((aligned(4096)));
static u8   l_hdr[16] __attribute__((aligned(16)));
static u8   l_st[16] __attribute__((aligned(16)));
static u8   l_buf[4096] __attribute__((aligned(4096)));
static u16  l_avail_idx, l_used_idx;

static volatile u32 *lecam(int b, int d, int f, int r)
{
    return (volatile u32 *)(L_PCI_ECAM_BASE + ((u64)b << 20) + ((u64)d << 15)
                            + ((u64)f << 12) + (r & 0xffc));
}

int vbl_init(void)
{
    l_io = 0;
    for (int b = 0; b < 16 && !l_io; b++)
        for (int d = 0; d < 32 && !l_io; d++)
            for (int f = 0; f < 8 && !l_io; f++) {
                u32 id = *lecam(b, d, f, L_PCI_VENDOR);
                if ((id & 0xffff) != L_VENDOR) continue;
                if (((id >> 16) & 0xffff) != L_BLK_TRANS) continue;
                /* 头类型在偏移 0x0E 的最低字节（早期写成 >>16 会读到 BAR0，把设备过滤掉） */
                if ((*lecam(b, d, f, L_PCI_HEADER) & 0x7f) != 0) continue;
                u32 bar0 = *lecam(b, d, f, L_PCI_BAR0);
                if (!(bar0 & 1)) continue;                 /* 只处理 I/O BAR 型 */
                l_io = L_PCI_IO_WINDOW + (bar0 & ~3u);
                u32 cmd = *lecam(b, d, f, L_PCI_CMD);
                *lecam(b, d, f, L_PCI_CMD) = cmd | L_PCI_CMD_IOSE | L_PCI_CMD_MSE | L_PCI_CMD_BME;
                lbar();
            }
    if (!l_io) return -1;

    w8(l_io + LQ_STATUS, 0);
    lbar();
    w8(l_io + LQ_STATUS, LST_ACK);
    w8(l_io + LQ_STATUS, LST_ACK | LST_DRIVER);
    (void)r32(l_io + LQ_HOST_FEAT);
    w32(l_io + LQ_GUEST_FEAT, 0);

    w16(l_io + LQ_QUEUE_SEL, 0);
    l_qnum = r16(l_io + LQ_QUEUE_NUM);
    if (l_qnum == 0 || l_qnum > 128) return -2;   /* 128 时 used 落在 ring+4096，8KB 够用 */

    for (u32 i = 0; i < sizeof(l_ring); i++) l_ring[i] = 0;
    l_avail_idx = l_used_idx = 0;

    w32(l_io + LQ_QUEUE_PFN, (u32)((u64)l_ring >> 12));
    w8(l_io + LQ_STATUS, LST_ACK | LST_DRIVER | LST_DRIVER_OK);

    u32 lo = r32(l_io + LQ_CONFIG);
    u32 hi = r32(l_io + LQ_CONFIG + 4);
    l_cap = ((u64)hi << 32) | lo;
    if ((r8(l_io + LQ_STATUS) & LST_FAILED) || l_cap == 0) return -3;
    return 0;
}

static int l_req(u32 type, u64 lba, void *buf, u32 nsec, int dev_writes)
{
    struct ldesc *d = (struct ldesc *)l_ring;
    u16 *av_flags = (u16 *)(l_ring + l_qnum * 16);
    u16 *av_idx = av_flags + 1;
    u16 *av_ring = av_idx + 1;
    u16 *us_flags = (u16 *)(l_ring + 4096);
    u16 *us_idx = us_flags + 1;

    struct lhdr *h = (struct lhdr *)l_hdr;
    h->type = type; h->rsv = 0; h->sector = lba;
    l_st[0] = 0xff;

    d[0].addr = (u64)l_hdr; d[0].len = 16; d[0].flags = LVDESC_NEXT; d[0].next = 1;
    d[1].addr = (u64)buf;   d[1].len = nsec * 512;
    d[1].flags = LVDESC_NEXT | (dev_writes ? LVDESC_WRITE : 0); d[1].next = 2;
    d[2].addr = (u64)l_st;  d[2].len = 1; d[2].flags = LVDESC_WRITE; d[2].next = 0;

    l_avail_idx++;
    av_ring[(l_avail_idx - 1) % l_qnum] = 0;
    lbar();
    *av_idx = l_avail_idx;
    lbar();
    w16(l_io + LQ_QUEUE_NOTIFY, 0);

    for (u64 spin = 0; spin < 40000000ull; spin++) {
        lbar();
        if (*us_idx != l_used_idx) {
            l_used_idx++;
            (void)r8(l_io + LQ_ISR);
            return (l_st[0] == 0) ? 0 : -3;
        }
    }
    return -4;
}

int vbl_read(u64 lba, void *buf, u32 nsec)
{
    u32 done = 0;
    while (done < nsec) {
        u32 chunk = (nsec - done > 8) ? 8 : (nsec - done);
        int r = l_req(LT_IN, lba + done, l_buf, chunk, 0);
        if (r < 0) return r;
        u8 *dst = (u8 *)buf + (u64)done * 512;
        if (dst != l_buf)
            for (u32 i = 0; i < chunk * 512; i++) dst[i] = l_buf[i];
        done += chunk;
    }
    return 0;
}

int vbl_write(u64 lba, const void *buf, u32 nsec)
{
    u32 done = 0;
    while (done < nsec) {
        u32 chunk = (nsec - done > 8) ? 8 : (nsec - done);
        const u8 *src = (const u8 *)buf + (u64)done * 512;
        if (src != l_buf)
            for (u32 i = 0; i < chunk * 512; i++) l_buf[i] = src[i];
        int r = l_req(LT_OUT, lba + done, l_buf, chunk, 1);
        if (r < 0) return r;
        done += chunk;
    }
    return 0;
}

u64 vbl_capacity_bytes(void) { return l_cap * 512ull; }

/* 与 modern 的 vb_request 同签名，供上层统一分发 */
int vbl_request(u32 type, u64 lba, void *buf, u32 nsec, int dev_writes)
{
    (void)dev_writes;
    if (nsec == 0 || nsec > 8) return -2;
    if (type == 4) return 0;                     /* VIRTIO_BLK_T_FLUSH：无后端缓存，直接成功 */
    return l_req(type, lba, buf, nsec, type == LT_IN);   /* 读：设备写缓冲 → 置 WRITE 标志 */
}
