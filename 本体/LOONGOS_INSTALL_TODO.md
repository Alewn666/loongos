# LOONGOS_INSTALL — 把 ISO 做成安装盘（能装到硬盘并持久化）

目标：`install` 命令把系统装进一块 virtio 硬盘 → 之后**不插 ISO 也能从盘启动**，
文件系统落在盘上（关机不丢）。

## 关键技术选择（已定）
- **安装器不自己写 FAT**：构建 ISO 时已用 mtools 生成 `esp.img`（4MB FAT，内含 BOOTAA64.EFI + kernel.elf）
  → 通过 `objcopy -I binary` 把 `esp.img` 嵌进内核，安装器把它**按扇区原样拷进硬盘 ESP 分区**。
- **引导不写 UEFI 启动项**：ESP 里放 `\EFI\BOOT\BOOTAA64.EFI`（UEFI 规定的兜底路径），
  固件没找到启动项时会自动用它 → 省掉 boot manager 那套。
- **分区表用 MBR**（写 64 字节即可，GPT 要写 34 个扇区 + CRC，先不搞）：
  - p1：ESP（FAT，类型 0x0C），起始 LBA 2048，大小 = esp.img
  - p2：LoongOS 数据分区（类型 0x99），起始 LBA 4096，大小 = 盘剩余
- **磁盘驱动用 legacy virtio-pci**（QEMU/libvirt 默认的 transitional 设备同时支持）：
  寄存器在 I/O BAR（QMEMU virt 把 I/O 窗口映射在 0x3eff0000，已被现有 MMIO 映射覆盖），
  队列用 legacy 的「连续内存 + QUEUE_PFN」方式，比 modern 少一层 capability 解析。

## 里程碑（每步都要能独立验证）
- [x] M1 **virtio-blk 驱动**（modern virtio-pci；`test_virtio_blk.py` ALL PASS：容量/空盘/写读回）：枚举 PCI → 复位 → 特征协商 → 队列建立 → 单请求同步读写
      - 验证：`-drive file=/tmp/vb.img` 起 QEMU，串口打印 `容量`，读扇 0、写图案、读回比对
- [x] M2 **fs 移到磁盘**（`fs_format_disk`/`fs_mount_disk`/运行期 fs_nblocks；待随 M3 一起验）：`fs_attach_disk(part_lba, nblocks)`，bread/bwrite 走 virtio；
      `fs_nblocks` 从常量改成运行期（位图一块能管 32768 块 = 128MB）
      - 验证：格式化成盘 → 建文件 → 重启（同一块盘）→ 文件还在
- [x] M3 **installer 端到端验收通过**（2026-10-25 13:3x：`test_install.py` 三段全过——ISO 装机 → 只挂硬盘启动 `MOUNTED-DISK lba=4096 blocks=15872` → 写 /home/persist.txt → 重启 `cat` 回来 hello-persist）：MBR + 拷 esp.img + 格式化 p2；`install` 命令带 yes 确认
      - 验证：装完拔掉 ISO，只挂硬盘启动 → 进 shell；写文件 → 重启 → 文件还在
- [x] M4 收尾（2026-10-25）：`disks` 命令已有；留痕见 `docs/检查与修复记录.md` **第十八轮**（原计划写第十七轮，但第十七轮已被「屏幕边距」占用，顺延第十八轮）；ISO 重建归档（md5 见留痕）
      - 补：内核新增纯 ASCII 标记 `[install] DONE` / `[fs  ] MOUNTED-DISK`（自动化测试不再依赖中文串口，中文被降级成 `?` 时也能判）+ `[strchk]` rodata 自检行

## 风险 & 注意
- `install` 会**抹掉整块盘**：必须二次确认（输入 yes），并且只在 virtio 盘上做
- 一次只跑一个测试 VM（`qemu_guard.py` 已接入）
- 内核 .bss 离 USER_BASE 只剩 ~11KB：virtio 的队列/缓冲要精打细算（用 4KB 内的小缓冲，
  描述符环用静态数组但不能大过几 KB）
- 磁盘 IO 全程 512B 扇区，fs 是 4KB 块 → 一块 = 8 个连续扇区，一次请求搞定

## 实现过程中的四个坑（都已修，别再踩）
1. **BAR 类型位不可靠**：PCI 规范里 BAR 低 2 位是只读类型位，QEMU 按 size 掩码写入时把它清掉
   （写 0x1001 读回 0x1000）→ 用 `bar0 & 1` 判断"I/O 型"永远失败。改成只看地址是否已分配。
2. **legacy virtio 的 I/O 端口在 PCIe 上走不通**（没固件配根端口 I/O 窗口，读回全 0xFF）→ 改用 modern（MMIO）。
3. **modern common cfg 的特征寄存器是 select+value 两段式**（写 0x00 选低/高半，读 0x04），
   不是平坦 u64；当成 u64 读会得到 `feat=0x30006e54...` 这种垃圾，报"设备不支持 VIRTIO 1.0"。
4. **`virtio_pci_cap` 的 `bar` 字段在第 4 字节**（不是第 2），且 `PCI_STATUS` 在偏移 0x06
   不满足 ECAM 4 字节对齐 → 要读 0x04 再右移 16 位。
5. **stub 看不到 ISO9660**：它打开的是「自己所在的那个 FAT 卷」（El Torito 的 esp.img），
   所以安装载荷要做成 `payload.img`（256KB FAT）塞进那个卷里，而不是放 ISO 根目录。
