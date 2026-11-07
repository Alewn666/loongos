#!/bin/sh
# LoongOS 运行脚本：QEMU virt(gic-version=2) 启动内核，串口接终端
# 退出：Ctrl+A 然后 X
cd "$(dirname "$0")"
exec qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a57 -m 128 \
     -nographic -kernel kernel.elf
