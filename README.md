# 🐉 灵珑系统 · LoongOS（纯自研内核版）

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![C](https://img.shields.io/badge/C-AArch64%20%E8%A3%B8%E6%9C%BA-00599C?logo=c&logoColor=white)
![Release](https://img.shields.io/github/v/release/Alewn666/loongos?label=release)
![QEMU](https://img.shields.io/badge/%E8%BF%90%E8%A1%8C-QEMU%20virt%20%7C%20%E6%89%8B%E6%9C%BA-6DA9C7)

> 一个从 `boot.S` 第一行开始手写的 64 位 ARM 操作系统：没有借用现成内核，自己实现 GIC 中断（v2/v3 运行时探测）、调度、文件系统、ELF 加载、xHCI 多设备 HID、NVMe / virtio 磁盘与网卡、TCP，最后能在 QEMU 里开机、**默认进图形桌面**、用鼠标操作、在窗口里敲命令装系统——甚至装进安卓手机跑。

这不是 Linux 发行版，也不是教学用的玩具 demo。它是一套完整可启动的裸机内核：UEFI 从 `BOOTAA64.EFI` 接手，汇编入口建好页表跳进 `kernel.c`，之后 GICv2/v3 自适应中断、10Hz 时钟节拍、抢占调度、帧缓冲中英文点阵、渐变壁纸与多窗口桌面、USB 键盘/鼠标/平板、自带文件系统、原生 NVMe 驱动、用户态 ELF 程序，全部由仓库里的 C 和汇编代码提供。

`loongos` 与 [linglum-linux](https://github.com/Alewn666/linglum-linux) 是同一个「灵珑系统」的两条技术路线：那条用 Linux 内核 + BusyBox，这条连内核一起自己写，互不替代。

## 已经跑起来的能力

| 子系统 | 实现位置 | 状态 |
| --- | --- | --- |
| 启动与页表 | `boot.S` / `uefi_stub.c` / `linker.ld` / `BOOTAA64.EFI` | UEFI 引导 + 自研 PE stub；ISO El Torito 卷内 `\payload.img` 承载内核 |
| 中断与调度 | `kernel.c` / `smp.c` | GICv2/v3 运行时探测（读 PIDR2）、10Hz tick、抢占式多任务；SMP 代码已具备但**默认关闭**（见「诚实的边界」） |
| 显示 | `kernel.c` / `font8x16.h` / `font_cjk.h` / `gfx.c` | 8×16 英文 + 16×16 中文点阵；UEFI GOP **或** QEMU ramfb（裸 ELF 启动经 fw_cfg DMA 自建 800×600 显示底座）；渐变壁纸、顶栏、任务栏时钟、脏矩形重画 |
| 输入 | `usb_kbd.c` | xHCI **多设备** HID：boot 键盘(8B) + 相对鼠标(4B) + 绝对平板(6B) 可同挂一个控制器；报告层识别 Ctrl+Shift+F1 |
| 图形桌面 | `gfx.c` / `gfx.h` / `gfx_draw.h` / `desktop.c` | 裁剪 / 命中 / z 序 / 脏矩形纯函数层 + 5 窗口、鼠标指针、点击提焦点、拖标题栏、点 × 关闭、任务栏切换；**开始菜单 10 项**、安装系统向导（二次确认，10 秒超时自动撤销） |
| 图形终端 | `gfxterm.c` | 窗口内可跑**全部** shell 命令（含 `install yes`）；`uart_putc` 输出捕获进 12KB 历史缓冲，中英混排、UTF-8 输入、退格、滚轮翻页 |
| 文件系统 | `fs.c` | 自带 fs，内存盘 / 磁盘持久化 |
| 用户程序 | `kernel.c` / `user/*.c` | ELF 加载与 `run`；`write` / `open` / `read` / `close` / `lseek` / `exit` 等系统调用（`svc #0`）；`hello` `cat` `args` `bad` `hang` 五个样例 |
| 存储 | `virtio_blk.c` / `virtio_blk_legacy.c` / `virtio_scsi.c` / `nvme.c` / `pci.c` | virtio-blk modern/legacy 双路径；**原生 NVMe**（Admin/IO SQ-CQ、PRP、CC/CSTS、Identify），`install yes` 装盘后重启文件仍在；virtio-scsi transitional 因 64 位 BAR 风险主动放弃 |
| 随机数 | `virtio_rng.c` | virtio-rng 设备识别与就绪（UEFI 5 拓扑回归覆盖） |
| 网络 | `virtio_net.c` / `net.c` / `tcp.c` | virtio-net + TCP 协议栈 |
| 加密 | `crypto.c` / `test_crypto.c` | 登录口令校验与哈希 |
| 安装 | `build_iso.sh` / `build_image.sh` | 生成可写入硬盘的 ISO 与磁盘镜像；五方 md5 校验保证 ISO 内嵌内核与源码构建一致 |

## 从源码到开机

构建脚本做的事很直接：先把用户程序编成独立 ELF，用 `objcopy` 嵌成 blob，再和内核各编译单元一起链成 `kernel.elf`。

```bash
cd 本体
sh build.sh                # 需要 aarch64-linux-gnu- 工具链；内含 BSS 余量守卫（当前余量 12 KB）
sh run.sh                  # 最简：纯串口，QEMU virt (gic-version=2, cortex-a57, 128MB)
                           # 退出：Ctrl+A 松开，再按 X

sh make_disk.sh disk.img 8 # 生成带 LoongOS 数据分区(0x99)的 8MB 磁盘镜像
sh run_full.sh             # 满配：virtio-blk 磁盘 + ramfb 图形 + virtio-net 网卡
                           # VNC 客户端连 127.0.0.1:5901 看 800x600 屏幕
```

**有帧缓冲时开机默认直接进图形桌面**（UEFI GOP、QEMU ramfb 或真机显卡）：鼠标点击 / 拖标题栏 / 点开始菜单，终端窗口里可以直接敲命令，包括 `install yes` 装系统。**按 `Ctrl+Shift+F1` 返回串口终端**，在终端敲 `desktop` 可再次进入桌面。

串口一旦有输入，桌面会自动让位且**不消费**该字符，原样交给 shell——纯串口启动（`run.sh`，无 GOP）因此优雅降级成命令行，出现 `> ` 提示符。`help` 列出全部内置命令：`ls /`、`cat`、`write`、`touch` / `mv` / `stat`、管道 `|`（可接 `wc` / `grep` / `head`）、重定向 `>` 与 `<`、`su demo` 切换用户（口令见 `docs`）都可以试。中文可以直接从串口输入并写入文件。

满配启动时，空白数据分区会首次自动格式化并挂载（日志里有 `MOUNTED-DISK`），之后写进 `/home` 的文件关机再开仍在；`ping 10.0.2.2 3` 前先敲 `net` 让 DHCP 拿地址。联网常用命令：`net`、`net dhcp`、`ifconfig`、`ping <ip>`、`tcp get <ip> <端口>`，静态兜底 `net static <ip> [mask] [gw]`。

## 下载可安装 ISO

仓库按 `.gitignore` 不存 `*.iso`，最新可安装镜像在 [Releases](https://github.com/Alewn666/loongos/releases) 分发（附 `loongos.iso.md5`）。ISO 内含 UEFI 引导卷、`payload.img` 与 `kernel.elf`，用 `-cdrom loongos.iso` 从 AAVMF 启动后敲 `install yes` 即可装盘。

> 在 NAS 虚拟机里安装时，目标盘请挂 **VirtIO Block / VirtIO SCSI / NVMe** 之一；如果 `install` 报 `FAIL no-disk` 且 `scsi` 输出 `rc=1 present=0`，说明 VM 没挂内核认得的硬盘，或挂的是 SATA/AHCI。

## 在安卓手机上跑（附运行说明）

手机有引导锁、也没有裸机驱动，所以走的是 Termux + QEMU aarch64 虚拟机这条路：在 Termux 里装 `qemu-system-aarch64-headless`，把打包好的系统目录放进去，用 `start.sh` 启动，就能看到它真的开机、真的敲命令、真的存文件。

```bash
pkg install -y qemu-system-aarch64-headless tar
```

> ⚠️ `手机版/` 里只有 [说明.md](手机版/说明.md) 和一份修复记录——**运行所需的 `loongos-phone` 包（含 `start.sh`、`kernel.elf` 等）是构建产物，未纳入公开仓库**。请先按 `本体/build.sh` 构建内核，再照说明文档组装手机包；文档里记有演示登录账号。记得给 Termux 关掉电池优化。

## 桌面系统：从路线图到已落地

[桌面系统_架构与路线.md](本体/docs/桌面系统_架构与路线.md) 是早期对着代码写的五层架构与分期清单。P1（图形地基）与 P2（鼠标 + 图形化操作全部功能）现已落地，实际实现路径与早期设想略有不同——**没有引入 `kmalloc` 和独立进程**，全部是静态 BSS：

```
apps    开始菜单 / 安装向导 / 图形终端 / 系统信息 / 操作说明   ← 窗口内容（desktop.c 内）
wm      窗口池 / 焦点 / 拖拽 / z 序 / 命中分派                 ← desktop.c
gfx     裁剪栈 / fill / rect / line / text / damage            ← gfx.h + gfx.c（36 项宿主单测）
input   键盘 / 相对鼠标 / 绝对平板 / Ctrl+Shift+F1             ← usb_kbd.c 多设备 HID + kernel.c 输入桥
HAL     framebuffer(GOP/ramfb) │ USB xHCI │ 10Hz timer
```

- **P1 ✅**：渐变壁纸 + 顶栏 + 任务栏时钟、中英混排、脏矩形、开机默认进桌面、无 GOP 优雅降级。
- **P2 ✅**：三设备同挂枚举、鼠标指针、点击 / 拖拽 / 关闭 / z 序、开始菜单 10 项、安装向导、图形终端跑全部命令、退出键改为**仅 Ctrl+Shift+F1**。
- **P3+ 待做**：文件管理器、文本编辑器、窗口缩放与最小化、右键菜单、更换字库（见「诚实的边界」）。

实测截图见 [docs/screenshots/](本体/docs/screenshots/)，逐轮设计与排障记录见 `LOONGOS_DESKTOP_TODO.md` 与 `docs/检查与修复记录.md`。

## 测试是怎么推进的

这个内核按阶段加能力，测试也按阶段留了下来，改任何一处都能回归：

```bash
cd 本体
python3 test_stage4_fs.py          # 文件系统            28/28
python3 test_stage5_qemu.py        # QEMU 启动            35/35
python3 test_stage5b_elf.py        # ELF 用户程序         33/33
python3 test_stage5c_cwd.py        # 工作目录             20/20
python3 test_stage5d_redirect.py   # 重定向               31/31
cc -Wall -Wextra -Werror test_wm_host.c -o /tmp/t && /tmp/t   # 图形纯函数 36/36
cc -Wall -Wextra test_net_host.c -o /tmp/n && /tmp/n           # 网络协议栈 52/52
python3 test_kbd.py                # USB 键盘
python3 test_scsi_install.py       # SCSI / 安装
python3 test_iso_boot.py           # ISO 引导
bash test_uefi.sh                  # UEFI 5 拓扑：scsi 桥 / NVMe / virtio-rng / 综合
python3 test_nvme_install.py       # NVMe 装盘 → MOUNTED-DISK → 重启文件仍在  ALL PASS
python3 test_desktop_p2.py         # 桌面 P2 端到端 8/8：QMP 注入鼠标点击/拖拽、
                                   # Ctrl+Shift+F1，screendump 抓图比对
bash test_drivers.sh               # 直启 TCG 抓设备识别
```

另外还有 `bench_boot.py` 启动耗时基准，以及 `diag_serial.py`、`probe_mmio.py`、`probe_pte.py`、`probe_drag.py` 等一批排障脚本——写内核时踩过的坑基本都留了可复现的工具。测试脚本均用 `cd "$(dirname "$0")"`，clone 到任意目录都能跑。

## 目录结构

```
loongos/
├── 本体/                 # 内核源码、构建与测试（主体）
│   ├── boot.S kernel.c smp.c fs.c crypto.c net.c tcp.c
│   ├── usb_kbd.c virtio_*.c nvme.c pci.c virtio_rng.c uefi_stub.c
│   ├── gfx.h gfx.c gfx_draw.h desktop.c desktop.h gfxterm.c gfxterm.h
│   ├── font8x16.h font_cjk.h linker.ld auth_cred.h
│   ├── user/             # 用户态 ELF 样例程序
│   ├── build.sh build_iso.sh build_image.sh build_pe.py
│   ├── run.sh run_full.sh make_disk.sh   # 最简 / 满配启动 / 磁盘镜像
│   ├── test_stage*.py test_desktop_p2.py test_nvme_install.py
│   ├── test_uefi.sh test_drivers.sh test_wm_host.c test_net_host.c
│   ├── LOONGOS_*_TODO.md  # 各阶段任务清单（内核演进档案）
│   └── docs/             # 桌面架构、检查与修复记录、实测截图
└── 手机版/               # Termux + QEMU 运行说明与修复记录
```

## 诚实的边界

- 用户态目前提供 `write` / `open` / `read` / `close` / `lseek` / `exit` 等系统调用，还没有堆分配器，内存全是静态 BSS；EL0 有 30 秒看门狗防止用户程序卡死内核。桌面与窗口同样是**内核态静态结构**，应用不是独立进程，一个窗口崩溃会影响整个系统。
- **多核 SMP 默认关闭，不是「已支持多核」**：QEMU 加 `-machine virt,virtualization=on` 时 PSCI 能拉起 `cpu1` 并在线，但从核定时期中断进不了处理程序，`wfi` 会满速空转烧核（实测 2.5 秒涨 5000 万+ 次）。本轮已修好从核向量表的 128B 槽距（原 IRQ 槽错落在 `+0x14` 而非 `0x280`）与 PSCI conduit 探测（无 EL3/EL2 时优雅跳过，不再刷异常转储）；剩下的是 GIC CPU interface 打通。`-DSMP_ENABLE` 仅供调试。
- **磁盘别用 `virtio-scsi-pci` 测**：驱动对 transitional 设备主动放弃（真机 PCIe 上改写 64 位 BAR 会 external abort，代码里有 2026-10-25 实测记录）。QEMU 用 `-device virtio-blk-pci` 即走 virtio-blk 路径，8MB 盘能读写、能格式化、能持久挂载；NVMe 盘走 `nvme.c` 原生驱动，装盘与持久化已通过端到端验证。
- **AAVMF 没有 NVMe 引导驱动**：NVMe 盘只能由内核起来后识别，不能作为 UEFI 启动盘；安装仍从 ISO/光盘引导。
- **字库有已知缺陷**：`font_cjk.h` 里 `·`(U+00B7) 字形本身是两条竖线，部分框形字（口/回/窗）缺底横——是 885 个字形的历史数据问题，尚未整体更换；桌面文案已避开这些字。
- **桌面与鼠标只在 TCG 下实测**：沙箱无 `/dev/kvm`，`test_desktop_p2.py` 8/8 全在 QEMU TCG 完成；真机是 KVM，装机后建议确认一次「进桌面 → 鼠标拖拽 → Ctrl+Shift+F1 → install/net」。
- 输入是轮询式事件处理，没有触摸 / 多点触控 / 手势；窗口缩放、最小化、右键菜单仍在 P3 清单里。
- 目标平台是 QEMU `virt` 与标准 UEFI aarch64，未适配具体实体 ARM 板卡的板级驱动。
- 仓库里的 `BOOTAA64.EFI`、`kernel.elf` 与若干 `.o` 是构建产物，便于直接体验；源码以 `build.sh` 为准，新增编译产物不再纳入版本控制（见 `.gitignore`）。

## 文档索引

- [手机版/说明.md](手机版/说明.md) — 在安卓手机上从安装到敲命令
- [本体/LOONGOS_DESKTOP_TODO.md](本体/LOONGOS_DESKTOP_TODO.md) — 桌面 P1/P2 验收清单与 P3+ 待办
- [本体/docs/桌面系统_架构与路线.md](本体/docs/桌面系统_架构与路线.md) — 早期图形化架构与分期清单（其中「只有 write/exit 两个系统调用」的现状描述已过时）
- [本体/docs/检查与修复记录.md](本体/docs/检查与修复记录.md) — 逐轮设计取舍与排障实录
- `LOONGOS_*_TODO.md` 与 `docs/检查与修复记录*.md` — 各阶段任务清单

## 许可证

MIT — 见 [LICENSE](LICENSE)。本项目由 NAS 归档包整理后公开发布。
