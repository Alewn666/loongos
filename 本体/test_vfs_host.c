/* 第五十六轮 VFS 宿主单测：挂载表 / 最长前缀 / devfs / procfs / 负例
 * 编译：cc -std=gnu11 -Wall -Wextra -Werror -O2 -o /tmp/test_vfs \
 *          test_vfs_host.c vfs.c && /tmp/test_vfs
 */
#include <stdio.h>
#include <string.h>
typedef unsigned long      u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
#include "vfs.h"

static int passed = 0, failed = 0;
static void ok(int c, const char *name)
{
    if (c) { passed++; }
    else { failed++; printf("[FAIL] %s\n", name); }
}
static void ok2(int c, const char *name, int a, int b)
{
    if (c) { passed++; }
    else { failed++; printf("[FAIL] %s (a=%d b=%d)\n", name, a, b); }
}

/* procfs 数据源：可控假数据，验证 /proc 读的是真实回调而非常量 */
static u32 s_mem_tot = 32768, s_mem_free = 12345, s_uptime = 777, s_ncpu = 4;
static u32 f_tot(void) { return s_mem_tot; }
static u32 f_free(void) { return s_mem_free; }
static u32 f_up(void) { return s_uptime; }
static u32 f_ncpu(void) { return s_ncpu; }
static u64 f_wall(void) { return 0; }
static struct procfs_source SRC = {
    f_tot, f_free, f_up, f_wall, "LoongOS 56.0-vfs (aarch64)", f_ncpu
};

static int slen(const char *s) { return (int)strlen(s); }

int main(void)
{
    vfs_init();
    ok(vfs_mount_count() == 0, "初始挂载表为空");
    ok(!vfs_mounted("/proc"), "初始 /proc 未挂载");

    /* 负例：非法路径 */
    ok(vfs_mount("proc", &g_procfs_ops, 0) < 0, "相对路径挂载被拒");
    ok(vfs_mount("/", &g_procfs_ops, 0) < 0, "根目录挂载被拒");
    ok(vfs_mount(0, &g_procfs_ops, 0) < 0, "空路径挂载被拒");

    /* 挂载 procfs / devfs */
    ok(vfs_mount("/proc", &g_procfs_ops, 0) == 0, "挂载 /proc 成功");
    ok(vfs_mount("/dev", &g_devfs_ops, 0) == 0, "挂载 /dev 成功");
    ok(vfs_mount_count() == 2, "挂载计数=2");
    ok(vfs_mount("/proc", &g_procfs_ops, 0) < 0, "重复挂载 /proc 被拒");
    procfs_set_source(&SRC);

    char b[512];
    int n;

    /* ---- procfs ---- */
    n = vfs_read_path("/proc/meminfo", (u8 *)b, sizeof b); b[n > 0 ? n : 0] = 0;
    ok(n > 0 && strstr(b, "MemTotal:") && strstr(b, "32768") &&
       strstr(b, "12345") && strstr(b, "kB"),
       "meminfo 含真实 MemTotal/MemFree 且来自回调");
    s_mem_free = 999;
    n = vfs_read_path("/proc/meminfo", (u8 *)b, sizeof b); b[n > 0 ? n : 0] = 0;
    ok(n > 0 && strstr(b, "999") && !strstr(b, "12345"),
       "改数据源后 meminfo 随之变化（非常量）");
    s_mem_free = 12345;

    n = vfs_read_path("/proc/uptime", (u8 *)b, sizeof b); b[n > 0 ? n : 0] = 0;
    ok(n > 0 && strstr(b, "777.00"), "uptime 777 秒正确");
    n = vfs_read_path("/proc/version", (u8 *)b, sizeof b); b[n > 0 ? n : 0] = 0;
    ok(n > 0 && strstr(b, "56.0-vfs"), "version 字符串正确");
    n = vfs_read_path("/proc/cpuinfo", (u8 *)b, sizeof b); b[n > 0 ? n : 0] = 0;
    ok(n > 0 && strstr(b, "aarch64") && strstr(b, "4"), "cpuinfo 架构+核数正确");
    n = vfs_read_path("/proc/mounts", (u8 *)b, sizeof b); b[n > 0 ? n : 0] = 0;
    ok(n > 0 && strstr(b, "/proc") && strstr(b, "/dev") &&
       strstr(b, "procfs") && strstr(b, "devfs"),
       "mounts 列出两个真实挂载点");

    /* 不存在的 proc 文件 */
    ok(vfs_read_path("/proc/nope", (u8 *)b, sizeof b) < 0, "proc 不存在文件读失败");
    /* 未挂载前缀不受理 */
    ok(vfs_read_path("/etc/hostname", (u8 *)b, sizeof b) < 0,
       "未挂载的 /etc 交回老路径（VFS 不接管）");
    ok(vfs_read_path("/nope", (u8 *)b, sizeof b) < 0, "/nope 不受理");

    /* stat */
    struct vstat st;
    ok(vfs_stat_path("/proc/meminfo", &st) == 0 && st.type == 2, "meminfo stat 是文件");
    ok(vfs_stat_path("/proc", &st) == 0 && st.type == 1, "/proc stat 是目录");

    /* ---- devfs ---- */
    n = vfs_read_path("/dev/null", (u8 *)b, sizeof b);
    ok(n == 0, "dev/null 读 0 字节");
    n = vfs_read_path("/dev/zero", (u8 *)b, 64);
    ok(n == 64, "dev/zero 读满 64 字节");
    int zero_ok = 1; for (int i = 0; i < n; i++) if (b[i]) zero_ok = 0;
    ok(zero_ok, "dev/zero 全零");
    ok(vfs_read_path("/dev/nope", (u8 *)b, sizeof b) < 0, "dev 不存在节点读失败");

    /* ---- 最长前缀匹配：嵌套挂载 ---- */
    ok(vfs_mount("/proc/sys", &g_procfs_ops, 0) == 0, "嵌套挂载 /proc/sys 成功");
    struct vfsmount *m; char rel[128];
    ok(vfs_resolve("/proc/sys", &m, rel, sizeof rel) == 1 &&
       slen(m->path) == 9 && slen(rel) == 0,
       "最长前缀命中 /proc/sys 且 rel 为空");
    ok(vfs_resolve("/proc/meminfo", &m, rel, sizeof rel) == 1 &&
       slen(m->path) == 5 && strcmp(rel, "meminfo") == 0,
       "普通 /proc/meminfo 命中 /proc 且 rel=meminfo");
    ok(vfs_resolve("/procx", &m, rel, sizeof rel) == 0,
       "/procx 不整段匹配 /proc 前缀");
    ok(vfs_umount("/proc/sys") == 0 && vfs_mount_count() == 2, "umount 嵌套点成功");

    /* ---- umount 后交回老路径 ---- */
    ok(vfs_umount("/proc") == 0, "umount /proc 成功");
    ok(!vfs_mounted("/proc"), "umount 后 /proc 未挂载");
    ok(vfs_read_path("/proc/meminfo", (u8 *)b, sizeof b) < 0,
       "umount 后 /proc/meminfo 不再由 VFS 受理");
    ok(vfs_umount("/proc") < 0, "重复 umount 被拒");

    /* ---- 挂载表满负例 ---- */
    vfs_init();
    int mounted = 0;
    char p[8];
    for (u32 i = 0; i < VFS_MMAX + 3; i++) {
        snprintf(p, sizeof p, "/m%u", i);
        if (vfs_mount(p, &g_procfs_ops, 0) == 0) mounted++;
    }
    ok2(mounted == (int)VFS_MMAX, "挂载表满后拒绝新增", mounted, (int)VFS_MMAX);
    ok(vfs_mount_count() == VFS_MMAX, "满表计数正确");

    printf("\n[test_vfs] %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
