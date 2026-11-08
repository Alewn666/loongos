#!/bin/bash
# 真实 UEFI 启动验证（AAVMF + ISO）。复现真机拓扑：光盘与硬盘都是 scsi（而非 -cdrom 伪装的
# virtio-blk），内核走 vs_init 而非被 CD 的 virtio-blk 短路。每张盘都通过 scsi-cd 启动 ISO，
# 再叠加测试特定设备。验证 scsi 多级桥 / NVMe / virtio-rng 识别与 install 装盘。
#
# ⚠️ 拓扑约定（2026-11-08 修正）：内核 vs_init 只认「第一个」virtio-scsi 控制器，并在其
#    LUN 里优先挑硬盘(target,lun)。所以每个用例只放**一个带硬盘的 scsi 控制器**（光盘也挂它上），
#    避免「CD 控制器先于硬盘控制器被枚举 → 漏掉硬盘」的局限。NVMe 用独立镜像 disk_nvme.img，
#    不与 scsi-hd 的 disk.img 抢写锁。
set -u
cd "$(dirname "$0")"
IMG=${IMG:-disk.img}
IMG_NVME=${IMG_NVME:-disk_nvme.img}
ISO=${ISO:-iso/loongos.iso}
CODE=/usr/share/AAVMF/AAVMF_CODE.fd
LOG=$(mktemp /tmp/loongos_uefi.XXXXXX)
VARS=$(mktemp /tmp/loongos_vars.XXXXXX)
KILL_AFTER=${KILL_AFTER:-45}

run() {
  local tag="$1"; shift
  echo "==================== $tag ===================="
  rm -f "$LOG" "$VARS"; cp /usr/share/AAVMF/AAVMF_VARS.fd "$VARS"
  timeout "$KILL_AFTER" qemu-system-aarch64 \
    -machine virt,gic-version=3 -cpu cortex-a57 -m 256M -smp 1 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    "$@" \
    -serial file:"$LOG" -display none -monitor none >/dev/null 2>&1 || true
  grep -E "\[disk\]|\[scsi\]|\[nvme\]|\[rng\]|扫描总线|MOUNTED|内存盘|MB \(|FAIL|就绪|容量|rc=|候选|使用 target|安装" "$LOG" | head -40
  echo "---- tail ----"; tail -4 "$LOG"; echo
}

# 0) 基线：单 scsi 控制器在 bus0（无桥），CD + 硬盘同挂 → 验证 scsi 基本识别 + CD/HDD 区分
run "scsi 无桥(基线)" \
  -drive file=$ISO,if=none,format=raw,id=cd0 -device virtio-scsi-pci,id=scsi,bus=pcie.0 \
  -device scsi-cd,drive=cd0,bus=scsi.0 \
  -drive file=$IMG,if=none,format=raw,id=d0 -device scsi-hd,drive=d0,bus=scsi.0

# 1) scsi 多级桥：单 scsi 控制器在 bus0->rp0->rp1 之后（真机根因场景），CD + 硬盘同挂
run "scsi 多级桥(真机场景)" \
  -device pcie-root-port,id=rp0,bus=pcie.0,chassis=1 \
  -device pcie-root-port,id=rp1,bus=rp0,chassis=2 \
  -device virtio-scsi-pci,id=scsi,bus=rp1 \
  -drive file=$ISO,if=none,format=raw,id=cd0 -device scsi-cd,drive=cd0,bus=scsi.0 \
  -drive file=$IMG,if=none,format=raw,id=d0 -device scsi-hd,drive=d0,bus=scsi.0

# 2) NVMe 原生盘（挂在根端口后），CD 走 scsihost 启动 ISO；NVMe 才是装盘目标
run "NVMe 原生盘" \
  -device virtio-scsi-pci,id=scsihost,bus=pcie.0 \
  -drive file=$ISO,if=none,format=raw,id=cd0 -device scsi-cd,drive=cd0,bus=scsihost.0 \
  -device pcie-root-port,id=rp0,bus=pcie.0,chassis=1 \
  -device nvme,drive=dp,serial=loongos-test,bus=rp0 \
  -drive file=$IMG_NVME,if=none,format=raw,id=dp

# 3) virtio-rng（与 scsi 硬盘共存）
run "virtio-rng" \
  -drive file=$ISO,if=none,format=raw,id=cd0 -device virtio-scsi-pci,id=scsi,bus=pcie.0 \
  -device scsi-cd,drive=cd0,bus=scsi.0 \
  -drive file=$IMG,if=none,format=raw,id=d0 -device scsi-hd,drive=d0,bus=scsi.0 \
  -device virtio-rng-pci

# 4) 综合：单 scsi 控制器在桥后（CD+硬盘）+ NVMe + rng 同场
run "综合 scsi桥+NVMe+rng" \
  -device pcie-root-port,id=rp0,bus=pcie.0,chassis=1 \
  -device pcie-root-port,id=rp1,bus=rp0,chassis=2 \
  -device virtio-scsi-pci,id=scsi,bus=rp1 \
  -drive file=$ISO,if=none,format=raw,id=cd0 -device scsi-cd,drive=cd0,bus=scsi.0 \
  -drive file=$IMG,if=none,format=raw,id=d0 -device scsi-hd,drive=d0,bus=scsi.0 \
  -device pcie-root-port,id=rp2,bus=pcie.0,chassis=3 \
  -device nvme,drive=dp,serial=loongos-test,bus=rp2 \
  -drive file=$IMG_NVME,if=none,format=raw,id=dp \
  -device virtio-rng-pci

rm -f "$LOG" "$VARS"
