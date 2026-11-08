#!/bin/bash
# 驱动验证：用 -kernel kernel.elf 直启（TCG），抓串口看设备识别
set -u
cd "$(dirname "$0")"
IMG=${IMG:-disk.img}
LOG=$(mktemp /tmp/loongos_log.XXXXXX)
KILL_AFTER=${KILL_AFTER:-18}

run() {
  local tag="$1"; shift
  echo "==================== $tag ===================="
  rm -f "$LOG"
  timeout "$KILL_AFTER" qemu-system-aarch64 \
    -machine virt,gic-version=3 -cpu cortex-a57 -m 128M -smp 1 \
    -kernel kernel.elf \
    -drive file="$IMG",if=none,format=raw,id=d0 \
    "$@" \
    -serial file:"$LOG" -display none -monitor none \
    >/dev/null 2>&1 || true
  # 关注驱动相关行
  grep -E "\[disk\]|\[scsi\]|\[nvme\]|\[rng\]|virtio-scsi|nvme 就绪|MOUNTED|内存盘|MB |FAIL" "$LOG" | head -40
  echo "---- tail ----"
  tail -6 "$LOG"
  echo
}

# 1) 基线：virtio-blk
run "virtio-blk" \
  -device virtio-blk-pci,drive=d0

# 2) virtio-scsi 多级桥后（bus0->rp0(sec1)->rp1(sec2)->scsi）：验证 BFS 桥遍历修复
run "virtio-scsi 多级桥" \
  -device pcie-root-port,id=rp0,bus=pcie.0,chassis=1 \
  -device pcie-root-port,id=rp1,bus=rp0,chassis=2 \
  -device virtio-scsi-pci,id=scsi,bus=rp1 \
  -device scsi-hd,drive=d0,bus=scsi.0

# 3) NVMe 原生盘
run "NVMe 原生盘" \
  -device nvme,drive=d0,serial=loongos-test

# 4) virtio-rng 熵源
run "virtio-rng" \
  -device virtio-blk-pci,drive=d0 \
  -device virtio-rng-pci

rm -f "$LOG"
