#!/bin/sh
# LoongOS 构建脚本：用户程序 ELF → objcopy 嵌成 blob → 与内核四件套链成 kernel.elf
set -e
cd "$(dirname "$0")"

CROSS=aarch64-linux-gnu-
CFLAGS="-nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-strict-aliasing -mcpu=cortex-a57 -O2 -Wall ${EXTRA_CFLAGS:-}"
# 用户程序：独立 ELF，链在用户区（kernel.c 的 run 校验 vaddr 必须在 [USER_BASE,USER_END)）
# ⚠️ USER_BASE 现为 0x40200000（用户区独立成 0x40200000-0x403FFFFF 整个 2MB 块），
#    用户程序链在 -Ttext=0x40201000；该值必须落在 [USER_BASE,USER_END) 内，
#    否则 cmd_run 报「入口不在用户区」。改 USER_BASE 时记得同步这里。
UCFLAGS="-nostdlib -ffreestanding -static -fno-stack-protector -fno-pie -O2 -Wall -Ttext=0x40201000 -Wl,--build-id=none -Wl,-z,max-page-size=0x1000"

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
rm -f boot.o kernel.o fs.o crypto.o usb_kbd.o virtio_blk.o virtio_blk_legacy.o virtio_scsi.o virtio_net.o net.o tcp.o smp.o kernel.elf
${CROSS}gcc $CFLAGS -c boot.S -o boot.o
${CROSS}gcc $CFLAGS -c kernel.c -o kernel.o
${CROSS}gcc $CFLAGS -c fs.c -o fs.o
${CROSS}gcc $CFLAGS -c crypto.c -o crypto.o
${CROSS}gcc $CFLAGS -c usb_kbd.c -o usb_kbd.o
${CROSS}gcc $CFLAGS -c virtio_blk.c -o virtio_blk.o
${CROSS}gcc $CFLAGS -c virtio_blk_legacy.c -o virtio_blk_legacy.o
${CROSS}gcc $CFLAGS -c virtio_scsi.c -o virtio_scsi.o
${CROSS}gcc $CFLAGS -c virtio_net.c -o virtio_net.o
${CROSS}gcc $CFLAGS -c net.c -o net.o
${CROSS}gcc $CFLAGS -c tcp.c -o tcp.o
${CROSS}gcc $CFLAGS -c smp.c -o smp.o
${CROSS}ld -T linker.ld -o kernel.elf boot.o kernel.o fs.o crypto.o usb_kbd.o virtio_blk.o virtio_blk_legacy.o virtio_scsi.o virtio_net.o net.o tcp.o smp.o \
    build/hello_blob.o build/bad_blob.o build/hang_blob.o build/cat_blob.o build/args_blob.o
${CROSS}objdump -h kernel.elf | head -12

# --- BSS 守卫：内核 .bss 绝不能顶到 USER_BASE ---------------------------------
# 背景（阶段6③ 血泪）：无堆分配器，所有窗口/图形缓冲都是静态数组。BSS 一旦涨过
# USER_BASE，编译零警告、开机正常，但 run 用户程序会踩进内核数据 → 静默全废。
# 这里从链接产物读真实 __bss_end，从 kernel.c 读 USER_BASE/USER_END，
# 从 UCFLAGS 读 -Ttext，三处交叉核对，任何一项越界就 fail fast。
BSS_END="0x$(${CROSS}nm kernel.elf | awk '/ __bss_end$/{print $1; exit}')"
USER_BASE=$(sed -n 's/^#define[[:space:]]*USER_BASE[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' kernel.c | head -1)
USER_END=$(sed -n 's/^#define[[:space:]]*USER_END[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' kernel.c | head -1)
TTEXT=$(printf '%s' "$UCFLAGS" | sed -n 's/.*-Ttext=\(0x[0-9A-Fa-f]*\).*/\1/p')
[ -n "$BSS_END" ] && [ "$BSS_END" != "0x" ] || { echo "守卫失败：读不到 __bss_end"; exit 1; }
[ -n "$USER_BASE" ] && [ -n "$USER_END" ] && [ -n "$TTEXT" ] || \
    { echo "守卫失败：USER_BASE / USER_END / -Ttext 解析不到"; exit 1; }
awk -v bss="$BSS_END" -v ub="$USER_BASE" -v ue="$USER_END" -v tt="$TTEXT" '
function h2d(s,   i, c, v, n) {
    n = 0; s = tolower(s); sub(/^0x/, "", s)
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1); v = index("0123456789abcdef", c) - 1
        if (v < 0) return -1
        n = n * 16 + v
    }
    return n
}
BEGIN {
    B = h2d(bss); U = h2d(ub); E = h2d(ue); T = h2d(tt)
    if (B < 0 || U < 0 || E < 0 || T < 0) { print "守卫失败：地址解析异常"; exit 1 }
    printf "[guard] __bss_end=%s  USER_BASE=%s  USER_END=%s  -Ttext=%s\n", bss, ub, ue, tt
    printf "[guard] BSS 余量 = %.1f KB（%d 字节）\n", (U - B) / 1024, U - B
    if (B >= U) { print "✗ __bss_end 已顶到/越过 USER_BASE：用户程序区会被踩穿"; exit 1 }
    if (T < U || T >= E) { print "✗ -Ttext 不在 [USER_BASE, USER_END) 内"; exit 1 }
    print "[guard] ✓ 三项检查通过"
}' || exit 1
echo "== kernel.elf 构建完成 =="
ls -la kernel.elf build/hello.elf build/bad.elf
