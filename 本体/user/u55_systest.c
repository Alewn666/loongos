/* /home/u55test —— 第五十五轮 syscall 扩容端到端冒烟（用户态 EL0 程序）
 *
 * 用法：run /home/u55test
 * 全部输出走 write(老接口) 打印 [R55] key=value / [R55] chk <name> ok|FAIL，
 * 由 test_round55.py 逐项正则解析判据（文档 §八「不接受串口打印说成功」——
 * 这里打印的是「程序实际拿到的 syscall 返回值/数据」，不是自报成功）。
 *
 * 覆盖 13 个新 syscall：getcwd/chdir/getpid/stat/fstat/dup/dup2/pipe/
 *                        mkdir/unlink/rename/kill/getdents
 * 以及 0~5 号老 syscall 的回归（open/read/lseek/close/write 逐字不变）。
 */
#include "u55_util.h"

static char g_dir[256];       /* getdents 缓冲 */
static char g_stat[64];       /* struct ustat = 24B，留足 */

/* struct ustat 与 ../usys.h 完全一致；用户态按字节偏移读，避免依赖头文件顺序。
   只读本程序实际断言用到的 3 个字段，其余留空以免 -Wall 报未使用函数。 */
static u32 st_size(const char *p) { return (u32)(*(const u32 *)(p + 0)); }
static u16 st_type(const char *p) { return (u16)(*(const u16 *)(p + 8)); }
static u16 st_uid (const char *p) { return (u16)(*(const u16 *)(p + 12)); }

void _start(void)
{
    int pass = 0, fail = 0;
    long r;

    u_puts("[R55] ==== u55test start ====\n");

    /* ---- 6 getcwd：开机在根 ---- */
    char cwd[64];
    r = u_getcwd(cwd, (long)sizeof cwd);
    kv("getcwd_rc", r);
    ks("getcwd_val", cwd);
    chk(&pass, &fail, "getcwd_root", r == 1 && u_eq(cwd, "/"));

    /* ---- 7 chdir：进 /etc 再回根 ---- */
    r = u_chdir("/etc");
    kv("chdir_etc_rc", r);
    char cwd2[64];
    long r2 = u_getcwd(cwd2, (long)sizeof cwd2);
    ks("getcwd_after_chdir", cwd2);
    chk(&pass, &fail, "chdir_ok", r == 0 && r2 == 4 && u_eq(cwd2, "/etc"));
    r = u_chdir("/");
    kv("chdir_root_rc", r);
    chk(&pass, &fail, "chdir_back", r == 0);
    /* 不存在的目录必须拒 */
    r = u_chdir("/no/such/dir");
    kv("chdir_bad_rc", r);
    chk(&pass, &fail, "chdir_reject_bad", r == -1);

    /* ---- 8 getpid：单进程模型恒为 1 ---- */
    long pid = u_getpid();
    kv("getpid", pid);
    chk(&pass, &fail, "getpid_1", pid == 1);

    /* ---- 9 stat：已知文件 /etc/hostname ---- */
    r = u_stat("/etc/hostname", g_stat);
    kv("stat_hostname_rc", r);
    kv("stat_size", (long)st_size(g_stat));
    kv("stat_type", (long)st_type(g_stat));
    kv("stat_uid",  (long)st_uid(g_stat));
    chk(&pass, &fail, "stat_ok", r == 0 && st_type(g_stat) == 2 && st_size(g_stat) > 0);
    r = u_stat("/no/such", g_stat);
    kv("stat_bad_rc", r);
    chk(&pass, &fail, "stat_reject_bad", r == -1);
    /* 相对路径经 cwd 解析 —— chdir 到 /etc 后 "hostname" 应命中 */
    u_chdir("/etc");
    r = u_stat("hostname", g_stat);
    kv("stat_relative_rc", r);
    chk(&pass, &fail, "stat_relative", r == 0 && st_size(g_stat) > 0);
    u_chdir("/");

    /* ---- 18 getdents：/etc 目录列表 ---- */
    r = u_getdents("/etc", g_dir, (long)sizeof g_dir);
    kv("getdents_etc_rc", r);
    ks("getdents_etc_val", g_dir);
    chk(&pass, &fail, "getdents_has_hostname", r > 0 && u_has(g_dir, "hostname"));
    r = u_getdents("/no/such", g_dir, (long)sizeof g_dir);
    kv("getdents_bad_rc", r);
    chk(&pass, &fail, "getdents_reject_bad", r == -1);

    /* ---- 14/19/16/15 mkdir / creat / rename / unlink（真实改文件系统） ----
     * 注意：fs_rename 明确「不支持移动目录」(返回 -3)，这是既有 fs 设计边界，
     * 所以 rename 用「文件」验证；mkdir/unlink 单独验证目录（空目录可删）。 */
    r = u_mkdir("/home/u55dir");
    kv("mkdir_rc", r);
    chk(&pass, &fail, "mkdir_ok", r == 0);
    r = u_stat("/home/u55dir", g_stat);
    kv("mkdir_stat_rc", r);
    kv("mkdir_stat_type", (long)st_type(g_stat));
    chk(&pass, &fail, "mkdir_is_dir", r == 0 && st_type(g_stat) == 1);
    r = u_mkdir("/home/u55dir");   /* 重复建应失败 */
    kv("mkdir_dup_rc", r);
    chk(&pass, &fail, "mkdir_reject_dup", r == -1);

    r = u_creat("/home/u55file");
    kv("creat_rc", r);
    chk(&pass, &fail, "creat_ok", r == 0);
    r = u_stat("/home/u55file", g_stat);
    kv("creat_stat_type", (long)st_type(g_stat));
    chk(&pass, &fail, "creat_is_file", r == 0 && st_type(g_stat) == 2);

    r = u_rename("/home/u55file", "/home/u55ren");
    kv("rename_rc", r);
    chk(&pass, &fail, "rename_ok", r == 0);
    r = u_stat("/home/u55file", g_stat);
    long old_gone = (r == -1) ? 1 : 0;
    r = u_stat("/home/u55ren", g_stat);
    long new_here = (r == 0 && st_type(g_stat) == 2) ? 1 : 0;
    kv("rename_old_gone", old_gone);
    kv("rename_new_here", new_here);
    chk(&pass, &fail, "rename_moved", old_gone && new_here);
    r = u_unlink("/home/u55ren");
    kv("unlink_rc", r);
    chk(&pass, &fail, "unlink_ok", r == 0);
    r = u_unlink("/home/u55ren");
    kv("unlink_again_rc", r);
    chk(&pass, &fail, "unlink_reject_again", r == -1);
    r = u_unlink("/home/u55dir");   /* 空目录可删 */
    kv("unlink_dir_rc", r);
    chk(&pass, &fail, "unlink_empty_dir", r == 0);

    /* ---- 2/3/5/4 老 syscall 回归：open/read/lseek/close 逐字不变 ---- */
    long fd = u_open("/etc/hostname");
    kv("open_fd", fd);
    chk(&pass, &fail, "open_fd3", fd >= 3 && fd < 6);
    char rb[64];
    long nr = u_read(fd, rb, 8);
    rb[nr > 0 ? nr : 0] = 0;
    kv("read_n", nr);
    ks("read_val", rb);
    chk(&pass, &fail, "read_ok", nr > 0);
    long sk = u_seek(fd, 0, 0);
    kv("lseek_set0", sk);
    chk(&pass, &fail, "lseek_ok", sk == 0);
    r = u_close(fd);
    kv("close_rc", r);
    chk(&pass, &fail, "close_ok", r == 0);

    /* ---- 10 fstat：按 fd 查 stat（与 stat 数值对账） ---- */
    long fd2 = u_open("/etc/hostname");
    r = u_fstat(fd2, g_stat);
    kv("fstat_rc", r);
    kv("fstat_size", (long)st_size(g_stat));
    chk(&pass, &fail, "fstat_ok", r == 0 && st_size(g_stat) > 0 && st_type(g_stat) == 2);
    r = u_fstat(99, g_stat);
    kv("fstat_bad_rc", r);
    chk(&pass, &fail, "fstat_reject_bad", r == -1);
    u_close(fd2);

    /* ---- 11/12 dup / dup2 ---- */
    int pfds[2];
    r = u_pipe(pfds);
    kv("pipe_rc", r);
    kv("pipe_rfd", pfds[0]);
    kv("pipe_wfd", pfds[1]);
    chk(&pass, &fail, "pipe_ok", r == 0 && pfds[0] >= 6 && pfds[1] >= 6 && pfds[0] != pfds[1]);

    /* pipe 往返：写端写、读端读回 */
    const char *msg = "PIPE-ROUNDTRIP";
    long mw = u_write(pfds[1], msg, 14);
    kv("pipe_write_n", mw);
    static char pb[32];
    long mr = u_read(pfds[0], pb, 14);
    pb[mr > 0 ? mr : 0] = 0;
    ks("pipe_read_val", pb);
    chk(&pass, &fail, "pipe_roundtrip", mw == 14 && mr == 14 && u_eq(pb, msg));

    /* dup(写端)：新 fd 也能写进同一 pipe */
    long dfd = u_dup(pfds[1]);
    kv("dup_fd", dfd);
    chk(&pass, &fail, "dup_ok", dfd >= 6 && dfd != pfds[0] && dfd != pfds[1]);
    mw = u_write(dfd, "ZZ", 2);
    long mr2 = u_read(pfds[0], pb, 2);
    pb[mr2 > 0 ? mr2 : 0] = 0;
    chk(&pass, &fail, "dup_shares_pipe", mw == 2 && mr2 == 2 && u_eq(pb, "ZZ"));
    u_close(dfd);

    /* dup2(写端 -> fd1)：stdout 重定向，写 fd1 的数据从 pipe 读端取回 */
    r = u_dup2(pfds[1], 1);
    kv("dup2_rc", r);
    chk(&pass, &fail, "dup2_ok", r == 1);
    u_write(1, "REDIR", 5);
    long mr3 = u_read(pfds[0], pb, 5);
    pb[mr3 > 0 ? mr3 : 0] = 0;
    ks("dup2_read_val", pb);
    chk(&pass, &fail, "dup2_stdout_redir", mr3 == 5 && u_eq(pb, "REDIR"));
    u_close(1);                      /* 恢复 console */
    u_close(pfds[0]);
    u_close(pfds[1]);

    /* ---- 17 kill：单进程模型，pid!=1 拒；pid==1 的 TERM 不真杀（留待 58 轮信号） ---- */
    r = u_kill(99, 15);
    kv("kill_badpid_rc", r);
    chk(&pass, &fail, "kill_reject_badpid", r == -1);
    r = u_kill(1, 15);
    kv("kill_self_rc", r);
    chk(&pass, &fail, "kill_self_ok", r == 0);

    /* ---- 越界指针保护：stat 的结构指针落在内核地址必须被拒 ---- */
    r = u_stat("/etc/hostname", (void *)0x40080000UL);
    kv("stat_badptr_rc", r);
    chk(&pass, &fail, "stat_reject_badptr", r == -1);

    kv("PASS", pass);
    kv("FAIL", fail);
    u_puts("[R55] ==== u55test done ====\n");
    sys_exit(fail ? 1 : 0);
}
