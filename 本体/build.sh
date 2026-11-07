#!/bin/sh
# LoongOS 构建脚本：用户程序 ELF → objcopy 嵌成 blob → 与内核四件套链成 kernel.elf
set -e
cd "$(dirname "$0")"

CROSS=aarch64-linux-gnu-
CFLAGS="-nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-strict-aliasing -mcpu=cortex-a57 -O2 -Wall ${EXTRA_CFLAGS:-}"
# 用户程序：独立 ELF，链在用户区（kernel.c 的 run 校验 vaddr 必须在 [USER_BASE,USER_END)）
# ⚠️ USER_BASE 现为 0x401F0000（2026-10-30 加网卡驱动后 bss 涨到 0x401E6400），
#    这里的 -Ttext 必须跟着 kernel.c 的 USER_BASE 走，否则 cmd_run 报「入口不在用户区」
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
echo "== kernel.elf 构建完成 =="
ls -la kernel.elf build/hello.elf build/bad.elf
