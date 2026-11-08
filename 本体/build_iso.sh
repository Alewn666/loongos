#!/bin/sh
# LoongOS 阶段5⑤ - 组装 UEFI 可启动 ISO
# 结构（两层都铺，AAVMF 才认）：
#   1) ISO9660 目录树里放真实文件  /EFI/BOOT/BOOTAA64.EFI + /kernel.elf
#      （AAVMF/EDK2 直接挂 ISO9660 树按标准路径找引导文件）
#   2) 同时嵌一个 FAT 引导镜像（El Torito EFI 平台、无仿真），
#      供按 El Torito FAT 读盘的固件兜底。
# 用法：sh build.sh && sh build_iso.sh   （依赖 BOOTAA64.EFI 与 kernel.elf 已构建）
set -e
cd "$(dirname "$0")"
mkdir -p iso
OUT=iso/loongos.iso
ESP=build/esp.img
STAGE=build/iso_tree

[ -f BOOTAA64.EFI ] || { echo "缺 BOOTAA64.EFI，先编 stub"; exit 1; }
[ -f kernel.elf ]   || { echo "缺 kernel.elf，先 sh build.sh"; exit 1; }

# ---- 1. ISO 根目录树（真实文件，-R Rock Ridge 保路径与大小写）----
rm -rf "$STAGE"
mkdir -p "$STAGE/EFI/BOOT"
cp BOOTAA64.EFI "$STAGE/EFI/BOOT/BOOTAA64.EFI"   # UEFI 标准引导路径（树里真文件）
cp kernel.elf   "$STAGE/kernel.elf"              # stub 从卷根读它

# ---- 2. 内嵌 El Torito FAT 镜像（同一套文件，兜底读盘路径）----
# ⚠️ 两个镜像职责不同（2026-11-08 修正装盘链路）：
#   payload.img = 4MB FAT（BOOTAA64.EFI + kernel.elf），install 时**原样写进硬盘
#                 ESP 分区**；装好的盘从硬盘启动时 stub 就从这个 FAT 卷读 kernel.elf。
#   esp.img     = 12MB FAT，El Torito 引导卷。除引导文件外还必须**内含
#                 \payload.img**：uefi_stub.c 用 loaded->DeviceHandle 打开的是这个
#                 FAT 卷（不是 ISO9660 树），只把 payload.img 放进 ISO 树会打印
#                 "[stub] no payload.img" → install 直接 FAIL no-esp-payload。
PAYLOAD=build/payload.img
rm -f "$PAYLOAD"
dd if=/dev/zero of="$PAYLOAD" bs=1M count=4 status=none
mformat -i "$PAYLOAD" ::
mmd -i "$PAYLOAD" ::/EFI ::/EFI/BOOT
mcopy -i "$PAYLOAD" BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$PAYLOAD" kernel.elf   ::/kernel.elf

rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=12 status=none
mformat -i "$ESP" ::
mmd -i "$ESP" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP" BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$ESP" kernel.elf   ::/kernel.elf
mcopy -i "$ESP" "$PAYLOAD"   ::/payload.img

# ISO9660 树里也各放一份（供按树读盘的固件兜底）
cp "$ESP"     "$STAGE/esp.img"
cp "$PAYLOAD" "$STAGE/payload.img"

# ---- 3. 组装：树=$STAGE，El Torito 指向树内的 esp.img ----
xorriso -as mkisofs -R -J -V LOONGOS     -e esp.img -no-emul-boot     -o "$OUT" "$STAGE" 2>&1 | tail -1

echo "✅ $OUT $(stat -c %s "$OUT") 字节"
echo "启动：qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a57 -m 128M \\"
echo "        -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \\"
echo "        -drive if=pflash,format=raw,file=build/AAVMF_VARS.fd \\"
echo "        -cdrom $OUT -nographic"
