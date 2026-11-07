#!/bin/sh
# LoongOS 阶段5⑤ - 编译 UEFI 引导 stub（BOOTAA64.EFI）
# 要点：必须用 gnu-efi 的 elf_aarch64_efi.lds 链接，否则 PE 头的
#       Subsystem / AddressOfEntryPoint 全是 0，固件认不出这是 EFI 应用
#       （表现为 "Not Found" / GRUB "cannot load image"）。
set -e
cd "$(dirname "$0")"

EFIINC=/usr/include/efi
EFILIB=/usr/lib

[ -f "$EFIINC/efi.h" ] || { echo "缺 gnu-efi 头文件（$EFIINC/efi.h）"; exit 1; }
[ -f "$EFILIB/elf_aarch64_efi.lds" ] || { echo "缺 $EFILIB/elf_aarch64_efi.lds"; exit 1; }

# 1) 编译：aarch64 EFI 应用的标准 CFLAGS（不碰固件栈保护、不留内核专属选项）
gcc -I"$EFIINC" -I"$EFIINC/aarch64" \
    -DGNU_EFI_USE_MS_ABI -fPIC -fshort-wchar -ffreestanding \
    -fno-stack-protector -fno-strict-aliasing -mno-outline-atomics \
    -Wall -O2 -c uefi_stub.c -o build/uefi_stub.o

# 2) 链接成 PE 共享对象：-T 用 gnu-efi 的 EFI 链接脚本（关键，定 Subsystem/Entry）
ld -nostdlib -znocombreloc -T "$EFILIB/elf_aarch64_efi.lds" -shared -Bsymbolic \
   -L"$EFILIB" "$EFILIB/crt0-efi-aarch64.o" build/uefi_stub.o \
   -o build/uefi_stub.so -lefi -lgnuefi

# 3) ELF → 合规 PE32+（自己生成，不用 objcopy）
# ⚠️ Debian binutils 2.40 的 objcopy 在 aarch64 上会产出"可选头残缺"的 PE
#    （段 RVA 从 0 起、无 .reloc、带 RELOCS_STRIPPED），EDK2 固件判 Unsupported。
#    改用 build_pe.py：整体平移 0x1000 + 生成真实 .reloc + 清 RELOCS_STRIPPED。
python3 build_pe.py build/uefi_stub.so BOOTAA64.EFI

echo "✅ BOOTAA64.EFI $(stat -c %s BOOTAA64.EFI) 字节"
