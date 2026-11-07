#!/bin/sh
# LoongOS 满配启动（2026-11-07 云电脑 QEMU 8.2 实证通过）
#
#   图形：QEMU ramfb + VNC。内核开机通过 fw_cfg 的 DMA 接口自建 800x600 显示底座，
#         裸 ELF 启动（不再需要 UEFI ISO）也能把 shell 画到屏幕上；
#         用任意 VNC 客户端连 127.0.0.1:5901 看画面。
#   磁盘：virtio-blk-pci + 带 0x99 分区的镜像；首次启动自动格式化并挂载，
#         之后写进 /home 的文件关机再开仍在。
#   网络：virtio-net-pci（QEMU user 模式），`ping 10.0.2.2 3` 可通。
#
# 退出：串口终端里 Ctrl+A 然后 X
# 相关：make_disk.sh 生成镜像；run.sh 是最简纯串口启动。
set -e
cd "$(dirname "$0")"

IMG=${IMG:-disk.img}
[ -f "$IMG" ] || sh ./make_disk.sh "$IMG" 8

# 源码比内核新就先构建（build.sh 内含 BSS 余量守卫）
if [ ! -f kernel.elf ] || [ kernel.c -nt kernel.elf ] || [ smp.c -nt kernel.elf ]; then
    echo "[run_full] 源码有更新，先构建 ..."
    sh ./build.sh >/dev/null
fi

exec qemu-system-aarch64 \
    -machine virt,gic-version=3 -cpu cortex-a57 -m 128M -smp 1 \
    -nographic -kernel kernel.elf \
    -drive file="$IMG",if=none,format=raw,id=d0 \
    -device virtio-blk-pci,drive=d0 \
    -device ramfb -vnc 127.0.0.1:1 \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0
