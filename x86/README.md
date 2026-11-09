# LoongOS x86_64 移植分支

> 独立源码树，**ARM 版（`本体/`）一行未动**。共享的只有 `gfx.c` / 字库头文件
> （纯 C，通过 `-I../本体` 复用），互不干扰。

## 这一版做到哪了（阶段 X0 · 地基，已全部实测通过）

| 能力 | 状态 | 验证 |
|---|---|---|
| GRUB multiboot2 引导 + BIOS/UEFI 双引导 ISO | ✅ | QEMU 启动，MB2 magic 校验通过 |
| 32 位保护模式 → IA-32e 长模式（自建 GDT / 4GB identity 页表） | ✅ | C 代码 @0x100000 跑起来 |
| 16550 串口 | ✅ | 全部日志可读 |
| IDT + 8259 PIC + PIT(100Hz) 中断体系 | ✅ | 中断驱动输入、节拍 1083 |
| Multiboot2 framebuffer tag（VBE/GOP）800×600×32 | ✅ | screendump 抓图，红条像素 `(255,0,0)` |
| **Linux 风格输入核心层**（`input.c`，架构无关） | ✅ | 宿主单测 **31/31** |
| i8042 PS/2 键鼠（中断上半部入队） | ✅ | QMP 注入 PS/2 事件 |
| 图形桌面（复用 gfx.c，中文点阵，3 窗口 + 光标 + 轨迹） | ✅ | 截图正常 |
| **纯移动（不点击）光标实时跟随** | ✅ | 端到端回归 PASS |
| 键盘回显 / Shift / Ctrl / typematic 自动重复 | ✅ | `hello` 回显、长按重复 PASS |
| Ctrl+Shift+F1 退出桌面 | ✅ | 组合键位图状态机 PASS |

`test_x86.py` 端到端 **6/6 ALL PASS**，退出诊断 `消费帧=51 丢失=0` ——
帧模型一帧不丢、不撕裂。

## 为什么输入层要按 Linux 重写

旧 ARM 版 `usb_kbd.c` 让每个 HID 设备直接写全局 `G_mx/G_my/G_mbtn`，多设备
轮流覆盖坐标和按钮位，于是「点不中」「光标漂」「要点击才知道位置」「固定
偏移」接连出现，修一个冒下一个。本分支照搬 Linux `drivers/input` 三层模型：

1. **设备层（中断上半部）** `ps2.c`：只读硬件字节 → `input_report()` 上报原始
   `input_event`，绝不碰任何全局坐标/按钮变量；
2. **核心层** `input.c`：用 **`EV_SYN` 原子帧** + SPSC 帧槽队列，生产者填满
   一帧才 `ready=1` 发布，消费者 `input_read_frame()` 一次取整帧、帧边界统一
   apply 到「按键位图 + 指针状态」，任何时刻都是一致快照；
3. **消费者** `desktop_x86.c`：只读帧边界快照绘图，不维护第二套坐标。

配套细节（都是 Linux 语义）：
- **按钮边沿上报** `input_report_btn_delta()`：只在按钮位变化时才发事件，
  从根上杜绝「后到的 btn=0 冲掉先前的按下」；
- **按键位图 + 修饰键状态机**：Ctrl/Shift/Alt 从位图查，不依赖当前报告里
  恰好带着 modifier 字节；
- **typematic**：PIT 节拍驱动，250ms delay / 40ms rate，重复事件用
  `value=2`（Linux autorepeat 语义），松键立即停止；
- 相对/绝对统一归约到单一指针状态，没有「2 秒时间窗口」这类隐式时序。

> `input.c` / `input.h` 完全架构无关，ARM 版后续可直接回用同一份实现。

## 构建与运行

```sh
cd x86
sh build_x86.sh          # 本机 gcc -m64 freestanding，产出 kernel_x86.elf
sh build_iso_x86.sh      # grub-mkrescue → iso/loongos_x86.iso（BIOS+UEFI）
gcc -O2 -Wall -o /tmp/ti test_input_host.c input.c && /tmp/ti   # 输入层宿主单测
python3 test_x86.py      # QEMU 端到端交互回归（需 qemu-system-x86_64 + Pillow）
```

QEMU 手动启动：

```sh
qemu-system-x86_64 -m 256M -cdrom iso/loongos_x86.iso \
  -display none -vga std -serial file:/tmp/s.log \
  -qmp unix:/tmp/q.sock,server=on,wait=off -no-reboot
```

## 环境依赖（已装）

`qemu-system-x86 ovmf grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools`

帧缓冲走 **Multiboot2 framebuffer tag**，不用 ARM 版的 fw_cfg ramfb —— QEMU
x86 pc 机默认 `fw_cfg` 无 DMA，而 ramfb 强制要求 DMA（`ramfb.c:130`）。

## 后续阶段（尚未做，如实列出）

| 阶段 | 内容 |
|---|---|
| X1 | 搬 `fs.c` / `gfxterm.c`，图形终端跑全部 shell 命令；开始菜单、窗口拖拽/关闭/z 序 |
| X2 | PCI ECAM 枚举 + virtio-blk/scsi + NVMe（把 ARM 版驱动换成 x86 的 `mfence`/port I/O 语义），`install yes` 装盘与重启持久化 |
| X3 | 网络栈（virtio-net + tcp）、用户态 ring3（`syscall/sysret`）、SMP/APIC |
| X4 | 与 ARM 版共用一份 `input.c`；真机 NAS x86 VM（KVM 硬件加速）实测 |

**当前版本不具备**：装盘、联网、文件持久化、完整 shell。它是「能进图形桌面、
鼠标键盘好用的地基」，不是 ARM 版的等价替代品。ARM 版仍是功能最全的主线。
