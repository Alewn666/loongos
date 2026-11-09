#!/bin/sh
# LoongOS x86_64 构建脚本（本机 gcc 直接出 x86_64 freestanding，无需交叉编译器）
# 用法：cd x86 && sh build_x86.sh
set -e
cd "$(dirname "$0")"

CC=gcc
LD=ld
CFLAGS="-m64 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
        -mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables \
        -fno-unwind-tables -O2 -Wall -Wextra ${EXTRA_CFLAGS:-}"

rm -f boot_x86.o kernel_x86.o kernel_x86.elf

echo "== [1/3] 汇编 boot_x86.S =="
$CC $CFLAGS -c boot_x86.S -o boot_x86.o

echo "== [1b/3] 汇编 idt_x86.S =="
$CC $CFLAGS -c idt_x86.S -o idt_x86.o

echo "== [2/3] 编译 C 源 =="
for f in kernel_x86.c platform.c ps2.c input.c font_shim.c desktop_x86.c; do
    echo "   cc $f"
    $CC $CFLAGS -I. -I../本体 -c "$f" -o "${f%.c}.o"
done

echo "== [2b/3] 复用 ARM 版 gfx.c（纯 C 绘图层）=="
$CC $CFLAGS -I. -I../本体 -c ../本体/gfx.c -o gfx.o

echo "== [3/3] 链接 kernel_x86.elf =="
OBJS="boot_x86.o idt_x86.o kernel_x86.o platform.o ps2.o input.o font_shim.o desktop_x86.o gfx.o"
$LD -m elf_x86_64 -T linker_x86.ld -o kernel_x86.elf $OBJS

echo "== 段布局 =="
objdump -h kernel_x86.elf | grep -E "\.text|\.rodata|\.data|\.bss|bootpkt|Idx" | head -12
echo "== 入口/关键符号 =="
nm -n kernel_x86.elf | grep -E " _start$| kernel_x86_main$| __bss_start$| __bss_end$| __fb_start$| pml4$| pd$| gdt$" | head -12
echo "== BSS 守卫：.bss 上界与帧缓冲不得重叠 =="
BSS_END="0x$(nm kernel_x86.elf | awk '/ __bss_end$/{print $1; exit}')"
FB_START="0x$(nm kernel_x86.elf | awk '/ __fb_start$/{print $1; exit}')"
printf '   __bss_end=%s  __fb_start=%s\n' "$BSS_END" "$FB_START"
printf '   kernel_x86.elf %s 字节\n' "$(stat -c %s kernel_x86.elf)"
echo "== 构建完成 =="
