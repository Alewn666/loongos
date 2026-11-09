#!/bin/sh
# 组装 LoongOS x86_64 可引导 ISO（BIOS + UEFI 双引导，GRUB multiboot2）
# 依赖：sh build_x86.sh 先产出 kernel_x86.elf
set -e
cd "$(dirname "$0")"
mkdir -p iso

[ -f kernel_x86.elf ] || { echo "缺 kernel_x86.elf，先 sh build_x86.sh"; exit 1; }

STAGE=iso/iso_tree
rm -rf "$STAGE"
mkdir -p "$STAGE/boot/grub"
cp kernel_x86.elf "$STAGE/boot/kernel_x86.elf"
cp grub.cfg       "$STAGE/boot/grub/grub.cfg"

# grub-mkrescue 自动带 multiboot2 模块；-d 指定 BIOS+EFI 模块都能找到
grub-mkrescue -o iso/loongos_x86.iso "$STAGE" 2>&1 | tail -3

echo "== ISO =="
stat -c '%n %s 字节' iso/loongos_x86.iso
md5sum iso/loongos_x86.iso | tee iso/loongos_x86.iso.md5
echo "== 启动示例 =="
echo "  qemu-system-x86_64 -m 256M -cdrom iso/loongos_x86.iso -display none -serial file:/tmp/s.log -monitor none"
