#!/bin/sh
# LoongOS 构建脚本：用户程序 ELF → objcopy 嵌成 blob → 与内核四件套链成 kernel.elf
#
# 2026-11-09 内核优化第一轮：编译单元清单收敛为下面这一个 KERN_SRCS。
# 之前 rm 清单 / 编译行 / 链接行各写一份（共 3 处、20 个文件），新增驱动要改
# 3 处，漏一处就是陈旧 .o 混进来或链接缺符号。现在：
#   * KERN_SRCS 是唯一事实来源，OBJS 由它派生，rm / 编译 / 链接都用它；
#   * 顺序保持与历史完全一致（boot.S 必须第一，.text.boot 才落在最前）；
#   * 「未登记源文件检查」会在有 .c 没写进清单时直接 fail fast，杜绝静默漏编译。
# 新增一个驱动/命令模块：只需要在 KERN_SRCS 里加一个文件名（一处）。
set -e
cd "$(dirname "$0")"

CROSS=aarch64-linux-gnu-
CFLAGS="-nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-strict-aliasing -mcpu=cortex-a57 -O2 -Wall ${EXTRA_CFLAGS:-}"
# 用户程序：独立 ELF，链在用户区（kernel.c 的 run 校验 vaddr 必须在 [USER_BASE,USER_END)）
# ⚠️ USER_BASE 现为 0x40230000（2026-11-14 阶段10 为 48KB 共享 DMA 池上移 0x40218000→0x40230000），
#    用户程序链在 -Ttext=0x40240000（=USER_BASE+64KB）；该值必须落在 [USER_BASE,USER_END) 内，
#    否则 cmd_run 报「入口不在用户区」。改 USER_BASE 时记得同步这里。
UCFLAGS="-nostdlib -ffreestanding -static -fno-stack-protector -fno-pie -O2 -Wall -Ttext=0x40240000 -Wl,--build-id=none -Wl,-z,max-page-size=0x1000"

# ---- 内核编译单元：唯一事实来源（顺序即链接顺序，勿随意重排）----
KERN_SRCS="boot.S kernel.c fs.c crypto.c usb_kbd.c \
           blkdev.c virtio_blk.c virtio_blk_legacy.c virtio_scsi.c virtio_net.c \
           net.c tcp.c smp.c nvme.c virtio_rng.c pci.c \
           nic.c e1000.c rtl8139.c \
           gfx.c gfxterm.c desktop.c"
# 由源码名派生目标文件名（.S / .c → .o）
OBJS=""
for s in $KERN_SRCS; do
    OBJS="$OBJS ${s%.*}.o"
done

# 未登记源文件检查：本体目录下的 .c 若不属于下面这几类「非内核源码」，
# 就必须登记在 KERN_SRCS，否则新增模块会被静默跳过（最隐蔽的构建事故）。
# 白名单显式列出，避免靠命名通配误判：
#   cat.c          根目录里的用户程序旧副本（真正编译的是 user/cat.c）
#   gen_authhash.c 宿主工具，单独 cc 编译，不进内核镜像
#   uefi_stub.c    独立 UEFI 应用，由 build_iso.sh 单独编译进 ESP
NON_KERNEL="cat.c gen_authhash.c uefi_stub.c"
for f in *.c; do
    case "$f" in
        test_*)   continue ;;      # 宿主机单元测试
        *_host.c) continue ;;      # 同上
    esac
    printf '%s\n' "$NON_KERNEL" | tr ' ' '\n' | grep -qx "$f" && continue
    printf '%s\n' "$KERN_SRCS" | tr ' ' '\n' | grep -qx "$f" || {
        echo "✗ 源文件 $f 未登记到 build.sh 的 KERN_SRCS（新增模块请加进去）"
        exit 1
    }
done

# --- 用户程序 ---
rm -rf build && mkdir -p build
${CROSS}gcc $UCFLAGS -o build/hello.elf user/hello.c
${CROSS}gcc $UCFLAGS -o build/bad.elf   user/bad.c
${CROSS}gcc $UCFLAGS -o build/hang.elf  user/hang.c
${CROSS}gcc $UCFLAGS -o build/cat.elf   user/cat.c
${CROSS}gcc $UCFLAGS -o build/args.elf  user/args.c
# ELF 文件本身转目标文件（符号 _binary_<文件名>_start/_end，文件名里的 . 和 / 会变 _）
( cd build && ${CROSS}objcopy -I binary -O elf64-littleaarch64 -B aarch64 hello.elf hello_blob.o )
( cd build && ${CROSS}objcopy -I binary -O elf64-littleaarch64 -B aarch64 bad.elf  bad_blob.o  )
( cd build && ${CROSS}objcopy -I binary -O elf64-littleaarch64 -B aarch64 hang.elf hang_blob.o )
( cd build && ${CROSS}objcopy -I binary -O elf64-littleaarch64 -B aarch64 cat.elf  cat_blob.o  )
( cd build && ${CROSS}objcopy -I binary -O elf64-littleaarch64 -B aarch64 args.elf args_blob.o )
${CROSS}objdump -h build/hello.elf | head -8

# --- 内核（构建前清 .o，别让陈旧目标文件混进来）---
rm -f $OBJS kernel.elf
for s in $KERN_SRCS; do
    echo "   cc $s"
    ${CROSS}gcc $CFLAGS -c "$s" -o "${s%.*}.o"
done
${CROSS}ld -T linker.ld -o kernel.elf $OBJS \
    build/hello_blob.o build/bad_blob.o build/hang_blob.o build/cat_blob.o build/args_blob.o
${CROSS}objdump -h kernel.elf | head -12

# --- 启动期 fw_cfg DMA 缓冲「真实 RAM 归属」守卫（2026-11-11 二次勘误后重写）------
# 事故链（两个都栽在「用了弱判据」上，务必记住）：
#  ① P3.2（2026-11-10）：缓冲原在 .bss，插入 136B 队列变量后地址漂移，图形拓扑
#     开机 HANG（日志止于 [probe] A）。修复 = 显式初始化落 .data。
#  ② R 轮（2026-11-11）：误以为「.data 浮动=加功能必炸」，把缓冲钉到固定物理
#     地址 0x4007E000（内核加载 0x40080000 之前的低 RAM / QEMU loader-DTB 区）。
#     7 组实验全用「串口出现『进桌面』」当成功，但那句是 fwc_ramfb_setup() 返回后
#     无条件打印的假象 —— 实际 QEMU 根本没建 ramfb surface，screendump 恒为
#     640x480 纯黑，全套图形回归（P2/P2.1/P2.2/P3）画面差异全 0.000 而静默全红。
#     A/B 铁证：缓冲在内核镜像内的 .data(0x400ae480) / .bss(0x400b8778) 都能
#     screendump 出 800x600 彩色画面；0x4007E000 恒黑屏。
# 真正约束（本守卫钉死）：缓冲必须是 .data 显式初始化静态数组（不退回 .bss），
#   且 nm 绝对地址 >= 0x40080000（内核镜像加载区内、QEMU 已映射的普通 RAM）。
#   是否真能出画面不靠地址猜，由回归里的 test_framebuffer.py 用真实 screendump
#   （尺寸 800x600 + 颜色数 > 阈值）守住 —— 行为判据优先于静态地址约定。
DMA_SEG_FAIL=0
for sym in fwc_cfg_buf fwc_dma_desc; do
    line=$(${CROSS}nm kernel.elf | awk -v s="$sym" '$NF==s{print $1" "$2; exit}')
    addr=$(printf '%s' "$line" | awk '{print $1}')
    seg=$(printf '%s' "$line"  | awk '{print $2}')
    case "$seg" in
        d|D|r|R) ;;                       # .data / .rodata 均可
        *) echo "X fw_cfg DMA 缓冲 $sym 落在段 '$seg'（应为 .data 的 d/D，禁止退回 .bss 或改绝对地址宏）"; DMA_SEG_FAIL=1; continue ;;
    esac
    # 0x40080000 = 内核镜像加载起点；低于它的地址是 QEMU loader/DTB 暂存区，
    # fw_cfg DMA 在那里不会建立 ramfb surface（黑屏）。用字符串比较十六进制位数+
    # 字典序即可（都是 0x40 开头的 8 位十六进制）：>= 0x40080000。
    ge=$(awk -v a="0x$addr" '
        function h2d(s,  i,ch,v,n){n=0;s=tolower(s);sub(/^0x/,"",s);
          for(i=1;i<=length(s);i++){ch=substr(s,i,1);v=index("0123456789abcdef",ch)-1;if(v<0)return -1;n=n*16+v}return n}
        BEGIN{ print (h2d(a) >= 1074266112) ? "OK" : "LOW" }')   # 1074266112=0x40080000
    if [ "$ge" = "OK" ]; then
        printf '[guard] fw_cfg DMA 缓冲 %-14s 段=%s 地址=0x%s（>= 0x40080000 镜像内 RAM）✓\n' "$sym" "$seg" "$addr"
    else
        echo "X fw_cfg DMA 缓冲 $sym 地址 0x$addr < 0x40080000：落在内核镜像前低 RAM，ramfb 会黑屏"
        DMA_SEG_FAIL=1
    fi
done
[ "$DMA_SEG_FAIL" = 0 ] || {
    echo "X 启动期 fw_cfg DMA 缓冲真实 RAM 归属守卫未通过：见 kernel.c 里 2026-11-11 二次勘误注释"
    echo "  （新增代码后务必跑 test_framebuffer.py 用真实画面确认，别再用『进桌面』当成功）"
    exit 1
}

# --- 内核空间预算守卫（阶段三）：把「还剩多少空间」量化到每一次构建 -----------
# 背景（阶段6③ 血泪 + 阶段二新发现）：
#   * 无堆分配器，所有窗口/图形缓冲/设备 DMA 队列都是静态数组；BSS 一旦涨过
#     USER_BASE，编译零警告、开机正常，但 run 用户程序会踩进内核数据 → 静默全废。
#   * USER_BASE 之前真正的内核上界是 .nvmedma_end（NVMe 的 6×4KB DMA 页排在
#     .bss 和 32KB 内核栈之后），不是 __bss_end。
#   * 阶段二发现：.bss 内 4KB 对齐的设备缓冲与 4B 小标志变量混排，一个标志
#     变量落在 4096 边界槽就会把整条缓冲链顶后一页 —— 余量会在 ±4KB 间跳变，
#     光看「构建成功 / 四项检查通过」完全看不出来。教训：余量必须**每次构建
#     都打印、且与上一次对比**，否则新增功能悄悄吃掉空间无从察觉。
# 因此这里：① 打印分段预算表（各段大小 + DMA 对齐浪费 + 上界余量）；
# ② 三级阈值预警；③ 把结果写进 bss_budget.txt（纳入版本库，团队共享同一
# 基线，下次构建打印 delta）。四项硬检查语义保持不变，越界一律 fail fast。
SYM() { ${CROSS}nm kernel.elf | awk -v s="$1" '$NF==s{print $1; exit}'; }
BSS_END="0x$(SYM __bss_end)"          # .bss 真实终点（含 32KB 内核栈）
NVMA_END="0x$(SYM __nvmedma_end)"      # 内核真正上界
TS="0x$(SYM __text_start)";   TE="0x$(SYM __text_end)"
RS="0x$(SYM __rodata_start)"; RE="0x$(SYM __rodata_end)"
DS="0x$(SYM __data_start)";   DE="0x$(SYM __data_end)"
BS="0x$(SYM __bss_start)";    NS="0x$(SYM __nvmedma_start)"
USER_BASE=$(sed -n 's/^#define[[:space:]]*USER_BASE[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' kernel.c | head -1)
USER_END=$(sed -n 's/^#define[[:space:]]*USER_END[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' kernel.c | head -1)
TTEXT=$(printf '%s' "$UCFLAGS" | sed -n 's/.*-Ttext=\(0x[0-9A-Fa-f]*\).*/\1/p')
# 所有段边界符号必须解析到真实十六进制地址，缺一即 fail（避免 0x 空值被当成 0）
SEG_OK=1
for v in "$BSS_END" "$NVMA_END" "$TS" "$TE" "$RS" "$RE" "$DS" "$DE" "$BS" "$NS"; do
    case "$v" in 0x[0-9A-Fa-f]*) ;; *) SEG_OK=0 ;; esac
done
[ "$SEG_OK" = 1 ] || { echo "守卫失败：链接器段边界符号解析不全"; exit 1; }
[ -n "$USER_BASE" ] && [ -n "$USER_END" ] && [ -n "$TTEXT" ] || \
    { echo "守卫失败：USER_BASE / USER_END / -Ttext 解析不到"; exit 1; }
awk -v ts="$TS" -v te="$TE" -v rs="$RS" -v re="$RE" -v ds="$DS" -v de="$DE" \
    -v bs="$BS" -v bss="$BSS_END" -v ns="$NS" -v nvma="$NVMA_END" \
    -v ub="$USER_BASE" -v ue="$USER_END" -v tt="$TTEXT" -v bf="bss_budget.txt" '
function h2d(s,   i, c, v, n) {
    n = 0; s = tolower(s); sub(/^0x/, "", s)
    if (s == "") return -1
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1); v = index("0123456789abcdef", c) - 1
        if (v < 0) return -1
        n = n * 16 + v
    }
    return n
}
BEGIN {
    # ⚠️ 变量名刻意避开 awk 内置变量：RS 是「记录分隔符」，绝不能拿来存
    #    __rodata_start（早期踩过：RS 被覆盖成巨大数字后，getline < bf 会把
    #    整个基线文件当成一条记录，^margin_bytes= 匹配不上，delta 静默丢失）。
    TS=h2d(ts); TE=h2d(te); ROS=h2d(rs); ROE=h2d(re)
    DS=h2d(ds); DE=h2d(de); BS=h2d(bs); BE=h2d(bss)
    NS=h2d(ns); NE=h2d(nvma); U=h2d(ub); E=h2d(ue); T=h2d(tt)
    if (TS<0||TE<0||ROS<0||ROE<0||DS<0||DE<0||BS<0||BE<0||NS<0||NE<0||U<0||E<0||T<0) {
        print "守卫失败：地址解析异常"; exit 1
    }
    margin = U - NE                 # 后续新功能还能吃多少字节
    gap    = NS - BE                # .bss_end → .nvmedma_start 的 4KB 对齐浪费

    printf "[budget] ===== 内核空间预算（阶段三 / 每次构建量化）=====\n"
    printf "[budget]   .text        %8.1f KB  (0x%x)\n", (TE-TS)/1024, TE-TS
    printf "[budget]   .rodata      %8.1f KB  (0x%x)\n", (ROE-ROS)/1024, ROE-ROS
    printf "[budget]   .data        %8.1f KB  (0x%x)\n", (DE-DS)/1024, DE-DS
    printf "[budget]   .bss         %8.1f KB  (0x%x)  含 32KB 内核栈\n", (BE-BS)/1024, BE-BS
    printf "[budget]   .nvmedma     %8.1f KB  (0x%x)  6x4KB DMA 页\n", (NE-NS)/1024, NE-NS
    printf "[budget]   DMA 对齐浪费 %6.1f KB  (__bss_end -> .nvmedma_start)\n", gap/1024
    printf "[budget]   内核上界 = 0x%x   USER_BASE = %s\n", NE, ub
    if (margin >= 6144)      tier = "充裕 OK"
    else if (margin >= 4096) tier = "关注 WARN：新增功能请评估 .bss 对齐碎片"
    else if (margin > 0)     tier = "偏紧 WARN：建议按 docs 扩容指引腾挪"
    else                     tier = "越界 FAIL"
    printf "[budget] * 上界余量 = %.1f KB (%d B)  %s\n", margin/1024, margin, tier
    printf "[budget]   阈值: >=6KB 充裕 / [4,6)KB 关注 / (0,4)KB 偏紧 / <=0 禁止构建\n"

    # ---- 四项硬检查（历史语义不变，越界一律 fail fast）----
    printf "[guard] __bss_end=%s  __nvmedma_end=%s  USER_BASE=%s  USER_END=%s  -Ttext=%s\n", bss, nvma, ub, ue, tt
    if (BE >= U)       { print "X __bss_end 已顶到/越过 USER_BASE：用户程序区会被踩穿"; exit 1 }
    if (NE >= U)       { print "X __nvmedma_end 已顶到/越过 USER_BASE：NVMe DMA 区会踩穿用户程序区"; exit 1 }
    if (T < U || T >= E) { print "X -Ttext 不在 [USER_BASE, USER_END) 内"; exit 1 }
    if (NS < BE)       { print "X .nvmedma 起点早于 .bss 终点：段重叠"; exit 1 }
    print "[guard] OK 四项检查通过"

    # ---- 团队共享基线：相对上次的余量 delta（让 ±4KB 跳变立刻现形）----
    old = -1
    while ((getline ln < bf) > 0)
        if (ln ~ /^margin_bytes=/) { split(ln, a, "="); old = a[2] + 0 }
    close(bf)
    if (old >= 0)
        printf "[budget] 相对 bss_budget.txt 基线：余量 %+d B\n", margin - old
    printf "# 由 build.sh 自动生成，纳入版本库作为内核空间基线\n" > bf
    printf "margin_bytes=%d\n",    margin >> bf
    printf "kernel_upper=0x%x\n",  NE     >> bf
    printf "user_base=%s\n",       ub     >> bf
    printf "bss_bytes=%d\n",       BE-BS  >> bf
    printf "data_bytes=%d\n",      DE-DS  >> bf
    printf "nvmedma_bytes=%d\n",   NE-NS  >> bf
    printf "align_gap_bytes=%d\n", gap    >> bf
    close(bf)
}' || exit 1
echo "== kernel.elf 构建完成 =="
ls -la kernel.elf build/hello.elf build/bad.elf
