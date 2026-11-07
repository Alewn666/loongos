# LOONGOS_ISO — 阶段5⑤：UEFI 引导，虚拟机里用 ISO 安装并跑起来 ✅ 已完成

目标：`qemu-system-aarch64 -machine virt -drive if=pflash,...AAVMF_CODE.fd -cdrom loongos.iso`
能走完整 UEFI 引导 → 装载内核 → 点亮 banner + 进 shell。内核主体保持 `-kernel` 直载兼容。

- [x] 1. 装构建依赖（gnu-efi / xorriso / mtools）
- [x] 2. 写 UEFI stub（读 ISO 上 kernel.elf → 解析 ELF 装段 → ExitBootServices → 跳入口）
- [x] 3. 组装 UEFI 可启动 ISO（双层：ISO9660 真文件 + El Torito FAT）
- [x] 4. QEMU + AAVMF + -cdrom 启动，串口验证 banner + shell
- [x] 5. 测试脚本固化（test_iso_boot.py 11/11）+ 回归 + 留痕

## 关键结论（2026-10-24 打通）

**整条链卡了两小时的唯一真凶：`BOOTAA64.EFI` 不是合法 PE 映像**，不是 El Torito、
不是 FAT、不是 NVRAM、不是 AAVMF 不认光盘。诊断路径与被排除的假设见
`docs/检查与修复记录.md` 第十轮。

**解法：`build_pe.py` 自己生成合规 PE32+**（objcopy 在 aarch64 上产不出合规 PE）：
- 读 ELF PT_LOAD → 整体平移 +0x1000（PC 相对寻址不受影响）
- 从 SHT_RELA 收集 157 条 `R_AARCH64_RELATIVE` → 生成真正的 `.reloc`
- 清 `RELOCS_STRIPPED` + 写 `DataDirectory[5]`
- `_start` 的 `bl _relocate` → `mov x0,#0`（防双重位移）

**另两个必踩的坑**：
- `efi_main` 必须最先 `InitializeLib(...)`，否则 `gBS` 空指针 → 崩在数据段
- 平移后重定位 site 初值要预设成 `addend + 0x1000`

## 构建/验证命令
```sh
sh build.sh            # 内核
sh build_stub.sh       # stub → BOOTAA64.EFI（自动调 build_pe.py）
sh build_iso.sh        # 组装 iso/loongos.iso
python3 test_iso_boot.py   # 真机 ISO 冒烟 11 项
```

## 未做
- 真正「安装到磁盘」要等阶段 6（virtio-blk 持久化）；当前是内存盘，断电即失
- 飞牛虚拟机的 UEFI 启动选项需用户在界面上确认（ISO 侧已就绪）
