/* LoongOS 第五十四轮 —— PL031 真实墙钟实现。纯整数历法，零 libc / 零浮点。 */
#include "rtc.h"

static int g_tz_min = 480;      /* 默认 UTC+8（中国用户默认时区） */
static int g_valid  = 0;

void rtc_init(void)
{
    /* PL031 DR 在 QEMU virt 上电即给宿主 UTC 秒。读两次确认是「活」的：
       值非 0 且落在合理区间（2020..2100），避免把未实现的 MMIO 读成 0/全 1
       后当成 1970 年。全程不打印，golden_behavior.txt 启动日志逐字不变。 */
    u32 a = RTC_DR;
    for (volatile u32 spin = 0; spin < 200000u; spin++) { }
    u32 b = RTC_DR;
    g_valid = (a >= 1577836800u && a < 4102444800u && b >= a && b - a <= 2) ? 1 : 0;
}

u64 rtc_now(void) { return g_valid ? (u64)RTC_DR : 0UL; }
int rtc_valid(void) { return g_valid; }
void rtc_set_tz(int minutes)
{
    if (minutes < -720 || minutes > 840) return;   /* UTC-12 .. UTC+14 */
    g_tz_min = minutes;
}
int rtc_tz(void) { return g_tz_min; }

/* 民用历换算：Howard Hinnant civil_from_days / days_from_civil 的整数版，
   无查表、无闰年特判分支， Gregorian 400 年周期。epoch 为 UTC 秒。 */
void rtc_breakdown(u64 ep, int *y, int *m, int *d,
                   int *hh, int *mm, int *ss, int *wday)
{
    /* 先叠加时区偏移（可能为负，用有符号 64 位承接） */
    long long t = (long long)ep + (long long)g_tz_min * 60;
    long long days = t / 86400;
    long long rem  = t % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long long doe = (unsigned long long)(z - era * 146097);
    unsigned long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long Y = (long long)yoe + era * 400;
    unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long long mp = (5 * doy + 2) / 153;
    unsigned long long D = doy - (153 * mp + 2) / 5 + 1;
    unsigned long long M = (mp < 10) ? (mp + 3) : (mp - 9);
    if (M <= 2) Y += 1;

    if (y) *y = (int)Y;
    if (m) *m = (int)M;
    if (d) *d = (int)D;
    if (hh) *hh = (int)(rem / 3600);
    if (mm) *mm = (int)((rem / 60) % 60);
    if (ss) *ss = (int)(rem % 60);
    /* 1970-01-01 是星期四(4)，wday: 0=Sun */
    long long w = (days + 4) % 7;
    if (w < 0) w += 7;
    if (wday) *wday = (int)w;
}

u64 rtc_epoch_of(int y, int m, int d, int hh, int mm, int ss)
{
    long long Y = y;
    Y -= (m <= 2) ? 1 : 0;
    long long era = (Y >= 0 ? Y : Y - 399) / 400;
    unsigned long long yoe = (unsigned long long)(Y - era * 400);
    unsigned long long mp = (unsigned long long)(m + (m > 2 ? -3 : 9));
    unsigned long long doy = (153 * mp + 2) / 5 + (unsigned long long)d - 1;
    unsigned long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    long long t = days * 86400 + hh * 3600 + mm * 60 + ss;
    t -= (long long)g_tz_min * 60;      /* 抵消上面的时区偏移，回到 UTC */
    return (u64)(t < 0 ? 0 : t);
}

static void put2(char *p, int v)
{
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}
static void put4(char *p, int v)
{
    p[0] = (char)('0' + (v / 1000) % 10);
    p[1] = (char)('0' + (v / 100) % 10);
    p[2] = (char)('0' + (v / 10) % 10);
    p[3] = (char)('0' + v % 10);
}

void rtc_format(u64 epoch, char *out, u32 cap)
{
    if (!out || cap < 40) { if (out && cap) out[0] = 0; return; }
    int y = 1970, m = 1, d = 1, hh = 0, mi = 0, ss = 0, wd = 4;
    rtc_breakdown(epoch, &y, &m, &d, &hh, &mi, &ss, &wd);
    static const char *wdn[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    /* "YYYY-MM-DD HH:MM:SS Www UTC+HHMM" = 33 字符 + NUL */
    u32 o = 0;
    put4(out + o, y); o += 4; out[o++] = '-';
    put2(out + o, m); o += 2; out[o++] = '-';
    put2(out + o, d); o += 2; out[o++] = ' ';
    put2(out + o, hh); o += 2; out[o++] = ':';
    put2(out + o, mi); o += 2; out[o++] = ':';
    put2(out + o, ss); o += 2; out[o++] = ' ';
    const char *w = wdn[wd >= 0 && wd < 7 ? wd : 0];
    out[o++] = w[0]; out[o++] = w[1]; out[o++] = w[2];
    out[o++] = ' '; out[o++] = 'U'; out[o++] = 'T'; out[o++] = 'C';
    int tz = g_tz_min;
    out[o++] = tz >= 0 ? '+' : '-';
    if (tz < 0) tz = -tz;
    put2(out + o, tz / 60); o += 2;
    put2(out + o, tz % 60); o += 2;
    out[o] = 0;
}
