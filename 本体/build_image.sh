#!/bin/sh
# LoongOS 阶段5⑤ - 生成 GRUB 可加载的 arm64 Linux Image（kernel.img）
# 为什么不走 EFI stub：Debian binutils 2.40 的 objcopy 不会把 ELF 的
# R_AARCH64_RELATIVE 重定位转成 PE .reloc，产出的 PE 缺 .reloc 段，
# EDK2 固件判 "Unsupported"。改走 GRUB 的 linux 命令加载标准 arm64 Image，
# 内核零改动（_start 本来就做 EL2→EL1 + 设栈 + 清 bss + 装向量表）。
set -e
cd "$(dirname "$0")"

[ -f kernel.elf ] || { echo "缺 kernel.elf，先 sh build.sh"; exit 1; }

# 1) ELF → 扁平二进制（从 0x40080000 起按 LOAD 段铺开，bss 不含在内由 _start 清零）
objcopy -O binary kernel.elf build/kernel.bin

# 2) 加 arm64 Image 头（64 字节，Linux boot protocol）
python3 - <<'EOF'
import struct, os
raw = open('build/kernel.bin','rb').read()
# 关键：Image 头占 64 字节，内核正文必须仍然落在 0x40080000（链接地址，绝对寻址）。
# 所以让"头"落在 0x4007FFC0，正文 64 字节后正好 0x40080000。
# QEMU virt RAM 基址 = 0x40000000（2MB 对齐），text_offset = 0x7FFC0。
TEXT_OFFSET = 0x7FFC0
hdr = bytearray(64)
# code0: b +64 —— 跳过 64 字节头，落到真正的 _start（偏移 0x40）
struct.pack_into('<I', hdr, 0x00, 0x14000010)   # b .+64
struct.pack_into('<I', hdr, 0x04, 0x00000000)   # code1
struct.pack_into('<Q', hdr, 0x08, TEXT_OFFSET)  # text_offset
struct.pack_into('<Q', hdr, 0x10, 64 + len(raw))# image_size
struct.pack_into('<Q', hdr, 0x18, 0)            # flags（不可重定位，必须装载到 text_offset）
struct.pack_into('<Q', hdr, 0x20, 0)            # res2
struct.pack_into('<Q', hdr, 0x28, 0)            # res3
struct.pack_into('<Q', hdr, 0x30, 0)            # res4
struct.pack_into('<I', hdr, 0x38, 0x644d5241)   # magic "ARM\x64"
struct.pack_into('<I', hdr, 0x3c, 0)            # res5
open('build/kernel.img','wb').write(bytes(hdr) + raw)
print(f"kernel.bin={len(raw)}  kernel.img={64+len(raw)}")
EOF

echo "✅ build/kernel.img $(stat -c %s build/kernel.img) 字节"
