/* LoongOS 59.0-A 开发者 API 演示程序（u60_api）
 * 展示 loslib.h 的用法：系统信息、时钟、随机数、统计、休眠、进程信息。
 * 编译：$CROSS gcc $UCFLAGS -o build/u60api.elf user/u60_api.c
 * 运行：在 shell 里 `run u60api`（需先把 ELF 放进镜像 /bin 或路径）。
 */
#include "loslib.h"

static void line(const char *s) { los_puts(s); los_puts("\r\n"); }

int main(void)
{
    line("== LoongOS Developer API demo (59.0-A) ==");

    /* 1) 系统信息 */
    struct utsname u;
    if (los_uname(&u) == 0) {
        los_puts("  uname: "); los_puts(u.sysname); los_puts(" ");
        los_puts(u.release); los_puts(" "); los_puts(u.machine); los_puts("\r\n");
    } else line("  uname: FAIL");

    /* 2) 墙钟 + 单调钟 */
    struct utime wall, mono;
    if (los_clock_gettime(0, &wall) == 0) {
        los_puts("  wall  = "); los_putdec(wall.sec); los_puts(" s\r\n");
    }
    if (los_clock_gettime(1, &mono) == 0) {
        los_puts("  mono  = "); los_print_time(&mono); los_puts(" s\r\n");
    }

    /* 3) uptime */
    struct utime up;
    if (los_uptime(&up) == 0) {
        los_puts("  uptime= "); los_putdec(up.sec); los_puts(" s\r\n");
    }

    /* 4) 真随机数（若无 virtio-rng 设备会 -1，演示里如实打印） */
    unsigned char rnd[8];
    long rn = los_getrandom(rnd, sizeof rnd);
    los_puts("  rng   = ");
    if (rn > 0) { for (long i = 0; i < rn; i++) { los_puthex(rnd[i]); } }
    else los_puts("unavailable");
    los_puts("\r\n");

    /* 5) 文件系统统计 */
    struct ufsstat fs;
    if (los_fsstat(&fs) == 0) {
        los_puts("  fs    = used "); los_putdec(fs.used_bytes);
        los_puts(" / total "); los_putdec(fs.total_bytes); los_puts(" B\r\n");
    }

    /* 6) 堆内存统计 */
    struct umeminfo mi;
    if (los_meminfo(&mi) == 0) {
        los_puts("  heap  = used "); los_putdec(mi.used_bytes);
        los_puts(" peak "); los_putdec(mi.peak_bytes); los_puts(" B\r\n");
    }

    /* 7) 进程信息 */
    los_puts("  pid   = "); los_putdec(los_getpid());
    los_puts(" ppid  = "); los_putdec(los_getppid()); los_puts("\r\n");

    /* 8) 休眠 10ms（演示用，短到不卡 shell） */
    struct utime nap = { 0, 10000000ull };
    los_nanosleep(&nap);
    line("  nanosleep(10ms) ok");

    line("== API demo done ==");
    los_exit(0);
    return 0;
}
