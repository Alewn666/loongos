/* /home/u55sh —— 第五十五轮「用户态管道」演示程序（EL0）
 *
 * 用法：run /home/u55sh [关键词]        关键词默认 "hostname"
 *
 * 做一件文档 §六 第 55 轮要求的事：`ls | grep <kw> | wc` 全链路。
 * 但**不是在内核 shell 里跑**（那是 shell_dispatch_line 的 redir_buf 字符串
 * 搬运），而是这个用户态 ELF 自己用 syscall 装配：
 *
 *   stage1 ls   : getdents("/etc") 拿目录名，写到 pipe A（经 dup2 到 fd1）
 *   stage2 grep : 从 pipe A 读端读回，按关键词过滤行，写到 pipe B
 *   stage3 wc   : 从 pipe B 读端读回，数行/词/字节，输出到 console
 *
 * 两个 pipe 都是内核 usysfd.c 的环形对象（ring 由第五十四轮 kmalloc 分配），
 * dup2 把 stdout 重定向到写端 —— 数据流真的经过内核对象，不是内存里拼字符串。
 *
 * 单进程模型（fork/exec 是第 57 轮）下按「写满 → 关写端 → 读」分阶段执行，
 * 不阻塞；这是诚实标注的边界，不冒充多进程并发管道。
 */
#include "u55_util.h"

static char g_ls[1024];        /* stage1 目录项文本 */
static char g_mid[1024];       /* stage2 中间缓冲 */
static char g_grep[1024];      /* stage2 过滤结果 */
static char g_wc[1024];        /* stage3 读回缓冲 */

/* 追加一行 "name\n" */
static long append_line(char *dst, long at, long cap, const char *s)
{
    long n = u_sl(s);
    if (at + n + 1 >= cap) return at;
    for (long i = 0; i < n; i++) dst[at++] = s[i];
    dst[at++] = '\n';
    dst[at] = 0;
    return at;
}

/* 统计行数 / 词数（空白分隔） / 字节数 */
static void count3(const char *s, long *lines, long *words, long *bytes)
{
    long L = 0, W = 0, B = 0, inword = 0;
    for (long i = 0; s[i]; i++) {
        B++;
        char c = s[i];
        if (c == '\n') L++;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') inword = 0;
        else if (!inword) { W++; inword = 1; }
    }
    *lines = L; *words = W; *bytes = B;
}

void _start(void)
{
    int pass = 0, fail = 0;
    long argc = u_argc();
    const char *kw = (argc > 1) ? u_argv(1) : "hostname";
    u_puts("[SH] ==== u55sh pipeline start ====\n");
    u_puts("[SH] keyword="); u_puts(kw); u_puts("\n");

    /* ---------- stage1 ls：getdents 拿 /etc 目录文本 ---------- */
    long nls = u_getdents("/etc", g_ls, (long)sizeof g_ls - 1);
    if (nls < 0) nls = 0;
    g_ls[nls] = 0;
    long ls_lines = 0;
    for (long i = 0; g_ls[i]; i++) if (g_ls[i] == '\n') ls_lines++;
    kv("ls_bytes", nls);
    kv("ls_lines", ls_lines);
    chk(&pass, &fail, "ls_getdents", nls > 0 && ls_lines > 0);

    /* ---------- 建两个真 pipe ---------- */
    int a[2], b[2];
    long ra = u_pipe(a), rb = u_pipe(b);
    kv("pipeA_rfd", a[0]); kv("pipeA_wfd", a[1]);
    kv("pipeB_rfd", b[0]); kv("pipeB_wfd", b[1]);
    chk(&pass, &fail, "two_pipes", ra == 0 && rb == 0 &&
        a[0] != a[1] && b[0] != b[1] && a[0] != b[0]);

    /* ---------- stage1 输出：dup2 到 fd1，写进 pipe A ---------- */
    long d1 = u_dup2(a[1], 1);
    kv("dup2_ls_rc", d1);
    long w1 = u_write(1, g_ls, nls);       /* 重定向：数据进 pipe A 写端 */
    kv("ls_write_n", w1);
    chk(&pass, &fail, "ls_into_pipeA", d1 == 1 && w1 == nls);
    u_close(1);                            /* 恢复 console */
    u_close(a[1]);                         /* 关写端，pipe A 数据定稿 */

    /* ---------- stage2 grep：从 pipe A 读回，过滤，写进 pipe B ---------- */
    long nmid = u_read(a[0], g_mid, (long)sizeof g_mid - 1);
    if (nmid < 0) nmid = 0;
    g_mid[nmid] = 0;
    kv("grep_read_n", nmid);
    chk(&pass, &fail, "grep_read_pipeA", nmid == nls);

    /* 逐行过滤含关键词的行 */
    long gl = 0, src_lines = 0, kept = 0;
    {
        long i = 0;
        while (i < nmid) {
            long j = i;
            while (j < nmid && g_mid[j] != '\n') j++;
            g_mid[j] = 0;                 /* 临时截断成一行 */
            src_lines++;
            if (u_has(g_mid + i, kw)) {
                gl = append_line(g_grep, gl, (long)sizeof g_grep, g_mid + i);
                kept++;
            }
            g_mid[j] = '\n';
            i = j + 1;
        }
    }
    g_grep[gl] = 0;
    kv("grep_src_lines", src_lines);
    kv("grep_kept_lines", kept);
    kv("grep_bytes", gl);
    ks("grep_result", g_grep);
    chk(&pass, &fail, "grep_filter", src_lines == ls_lines && kept > 0);

    /* dup2 到 fd1，把过滤结果写进 pipe B */
    long d2 = u_dup2(b[1], 1);
    kv("dup2_grep_rc", d2);
    long w2 = u_write(1, g_grep, gl);
    kv("grep_write_n", w2);
    chk(&pass, &fail, "grep_into_pipeB", d2 == 1 && w2 == gl);
    u_close(1);
    u_close(b[1]);
    u_close(a[0]);                         /* pipe A 两端全关 → 对象回收 */

    /* ---------- stage3 wc：从 pipe B 读回，统计，输出到 console ---------- */
    long nwc = u_read(b[0], g_wc, (long)sizeof g_wc - 1);
    if (nwc < 0) nwc = 0;
    g_wc[nwc] = 0;
    long L = 0, W = 0, B = 0;
    count3(g_wc, &L, &W, &B);
    kv("wc_read_n", nwc);
    kv("wc_lines", L);
    kv("wc_words", W);
    kv("wc_bytes", B);
    /* wc 的行数必须等于 grep 保留行数、字节数等于 grep 输出字节数 */
    chk(&pass, &fail, "wc_matches_grep", nwc == gl && L == kept && B == gl);

    /* 用老接口把最终结果打印到 console（此刻 fd1 已是 console） */
    u_puts("[SH] wc: lines="); u_dec(L);
    u_puts(" words="); u_dec(W);
    u_puts(" bytes="); u_dec(B);
    u_puts("\n");

    u_close(b[0]);                         /* pipe B 两端全关 → 对象回收 */

    kv("PASS", pass);
    kv("FAIL", fail);
    u_puts("[SH] ==== u55sh pipeline done ====\n");
    sys_exit(fail ? 1 : 0);
}
