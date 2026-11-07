#!/bin/sh
# 生成一块给 LoongOS 用的 virtio-blk 磁盘镜像（带 0x99 数据分区的 MBR）
# 内核开机识别到 MBR 里的 0x99 分区后，空白分区会自动格式化并持久挂载；
# 之后写进 /home 的文件，关机再开还在（2026-11-07 QEMU 实测三段全过）。
#
# 用法: ./make_disk.sh [镜像路径] [大小MB，默认 8]
# 注意: 这是给 QEMU 用的稀疏文件，不要拿去覆盖真实磁盘。
set -e
cd "$(dirname "$0")"
IMG=${1:-disk.img}
MB=${2:-8}
python3 - "$IMG" "$MB" <<'PY'
import sys, struct
path, mb = sys.argv[1], int(sys.argv[2])
total = mb * 1024 * 1024
lba   = 2048                      # 数据分区起始扇区（1MB 对齐）
secs  = total // 512 - lba       # 数据分区扇区数
if secs < 8192:                  # 至少 4MB，fs_format_disk 的下限
    raise SystemExit('镜像太小：至少 6MB')
mbr = bytearray(512)
mbr[446 + 0] = 0x80              # 可启动标志
mbr[446 + 4] = 0xEF              # p1: ESP（占位，裸 ELF 启动用不到）
mbr[446 + 8:446 + 12] = struct.pack('<I', 2048)
mbr[446 + 12:446 + 16] = struct.pack('<I', 0)
mbr[462 + 4] = 0x99              # p2: LoongOS 数据分区（内核认这个类型）
mbr[462 + 8:462 + 12] = struct.pack('<I', lba)
mbr[462 + 12:462 + 16] = struct.pack('<I', secs)
mbr[510], mbr[511] = 0x55, 0xAA
with open(path, 'wb') as f:
    f.truncate(total)
    f.write(mbr)
print('%s: %d MB, p2 LBA=%d blocks=%d' % (path, mb, lba, secs // 8))
PY
