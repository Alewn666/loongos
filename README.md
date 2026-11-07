# 🐉 灵珑系统 · LoongOS（纯自研内核版）

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![C](https://img.shields.io/badge/C-AArch64%20%E8%A3%B8%E6%9C%BA-00599C?logo=c&logoColor=white)
![Release](https://img.shields.io/github/v/release/Alewn666/loongos?label=release)
![QEMU](https://img.shields.io/badge/%E8%BF%90%E8%A1%8C-QEMU%20virt%20%7C%20%E6%89%8B%E6%9C%BA-6DA9C7)

> 一个从 `boot.S` 第一行开始手写的 64 位 ARM 操作系统：没有借用现成内核，自己实现 GIC 中断（v2/v3 运行时探测）、调度、文件系统、ELF 加载、USB 键盘、virtio 磁盘/网卡和 TCP，最后能在 QEMU 里开机、登录、敲命令、存文件——甚至装进安卓手机跑。

这不是 Linux 发行版，也不是教学用的玩具 demo。它是一套完整可启动的裸机内核：UEFI 从 `BOOTAA64.EFI` 接手，汇编入口建好页表跳进 `kernel.c`，之后 GICv2/v3 自适应中断、10Hz 时钟节拍、抢占调度、帧缓冲中英文点阵、xHCI USB HID 键盘、自带文件系统、用户态 ELF 程序，全部由仓库里的 C 和汇编代码提供。

`loongos` 与 [linglum-linux](https://github.com/Alewn666/linglum-linux) 是同一个「灵珑系统」的两条技术路线：那条用 Linux 内核 + BusyBox，这条连内核一起自己写，互不替代。

## 已经跑起来的能力

| 子系统 | 实现位置 | 状态 |
| --- | --- | --- |
| 启动与页表 | `boot.S` / `uefi_stub.c` / `linker.ld` / `BOOTAA64.EFI` | UEFI 引导 + 自研 PE stub |
| 中断与调度 | `kernel.c` / `smp.c` | GICv2/v3 运行时探测（读 PIDR2，兼容 KVM）、10Hz tick、抢占式多任务、多核 SMP |
| 显示 | `kernel.c` / `font8x16.h` / `font_cjk.h` | GOP 帧缓冲，8×16 英文 + 16×16 中文点阵 |
| 输入 | `usb_kbd.c` | xHCI 枚举 + USB HID boot 键盘报告解析 |
| 文件系统 | `fs.c` | 自带 fs，内存盘 / 磁盘持久化 |
| 用户程序 | `kernel.c` / `user/*.c` | ELF 加载与 `run`；`write` / `open` / `read` / `close` / `lseek` / `exit` 等系统调用（`svc #0`）；`hello` `cat` `args` `bad` `hang` 五个样例 |
| 存储 | `virtio_blk.c` / `virtio_blk_legacy.c` / `virtio_scsi.c` | virtio 块设备与 SCSI 适配 |
| 网络 | `virtio_net.c` / `net.c` / `tcp.c` | virtio-net + TCP 协议栈 |
| 加密 | `crypto.c` / `test_crypto.c` | 登录口令校验与哈希 |
| 安装 | `build_iso.sh` / `build_image.sh` | 生成可写入硬盘的 ISO 与磁盘镜像 |

## 从源码到开机

构建脚本做的事很直接：先把用户程序编成独立 ELF，用 `objcopy` 嵌成 blob，再和内核四件套一起链成 `kernel.elf`。

```bash
cd 本体
sh build.sh                # 需要 aarch64-linux-gnu- 工具链
sh run.sh                  # QEMU virt (gic-version=2, cortex-a57, 128MB)
                           # 退出：Ctrl+A 松开，再按 X
```

启动后先看到内核 banner，出现 `login:` 输入 `root`，进入 `#` 就是它自己的 shell 了。`help` 列出全部内置命令，`ls /`、`cat`、重定向、`&` 后台作业都可以试。

## 在安卓手机上跑（附运行说明）

手机有引导锁、也没有裸机驱动，所以走的是 Termux + QEMU aarch64 虚拟机这条路：在 Termux 里装 `qemu-system-aarch64-headless`，把打包好的系统目录放进去，用 `start.sh` 启动，就能看到它真的开机、真的敲命令、真的存文件。

```bash
pkg install -y qemu-system-aarch64-headless tar
```

> ⚠️ `手机版/` 里只有 [说明.md](手机版/说明.md) 和一份修复记录——**运行所需的 `loongos-phone` 包（含 `start.sh`、`kernel.elf` 等）是构建产物，未纳入公开仓库**。请先按 `本体/build.sh` 构建内核，再照说明文档组装手机包；文档里记有演示登录账号。记得给 Termux 关掉电池优化。

## 桌面系统的路线

[桌面系统_架构与路线.md](本体/docs/桌面系统_架构与路线.md) 不是一句「以后支持图形界面」，而是对着现有代码写的五层架构与分期清单：

```
apps/   文件管理器 │ 编辑器 │ 终端 │ 贪吃蛇 │ 计算器     ← 应用（回调式）
wm.c    窗口表 │ 裁剪栈 │ 焦点 │ 拖动 │ 事件分发       ← 窗口系统
gfx.c   画矩形/线/圆 │ 文字 │ 位图 │ 脏矩形            ← 绘图原语
input.c 键盘 │ 鼠标 │ 统一事件队列                    ← 输入抽象
HAL     framebuffer(已有) │ USB xHCI(已有) │ timer(已有)
```

文档里逐项盘点了「已有 / 缺失」：帧缓冲、键盘、文件系统、定时器都在，缺鼠标、窗口管理、绘图抽象、应用框架和 `kmalloc`——所以它是一份能照着开工的清单。

## 测试是怎么推进的

这个内核按阶段加能力，测试也按阶段留了下来，改任何一处都能回归：

```bash
cd 本体
python3 test_stage4_fs.py          # 文件系统
python3 test_stage5_qemu.py        # QEMU 启动
python3 test_stage5b_elf.py        # ELF 用户程序
python3 test_stage5c_cwd.py        # 工作目录
python3 test_stage5d_redirect.py   # 重定向
python3 test_kbd.py                # USB 键盘
python3 test_scsi_install.py       # SCSI / 安装
python3 test_iso_boot.py           # ISO 引导
```

另外还有 `bench_boot.py` 启动耗时基准，以及 `diag_serial.py`、`probe_mmio.py`、`probe_pte.py` 等一批排障脚本——写内核时踩过的坑基本都留了可复现的工具。

## 目录结构

```
loongos/
├── 本体/                 # 内核源码、构建与测试（主体）
│   ├── boot.S kernel.c smp.c fs.c crypto.c net.c tcp.c
│   ├── usb_kbd.c virtio_*.c uefi_stub.c
│   ├── font8x16.h font_cjk.h linker.ld auth_cred.h
│   ├── user/             # 用户态 ELF 样例程序
│   ├── build.sh build_iso.sh build_image.sh build_pe.py run.sh
│   ├── test_stage*.py test_kbd.py test_scsi*.py bench_boot.py
│   ├── LOONGOS_*_TODO.md  # 各阶段任务清单（内核演进档案）
│   └── docs/             # 桌面架构、检查与修复记录
└── 手机版/               # Termux + QEMU 运行说明与修复记录
```

## 诚实的边界

- 用户态目前提供 `write` / `open` / `read` / `close` / `lseek` / `exit` 等系统调用，还没有堆分配器，内存全是静态 BSS；EL0 有 30 秒看门狗防止用户程序卡死内核。
- 图形界面、鼠标、窗口系统仍在路线图阶段；当前交互是帧缓冲 + 串口/USB 键盘。
- 目标平台是 QEMU `virt` 与标准 UEFI aarch64，未适配具体实体 ARM 板卡的板级驱动。
- 仓库里的 `BOOTAA64.EFI`、`kernel.elf` 与若干 `.o` 是构建产物，便于直接体验；源码以 `build.sh` 为准。

## 文档索引

- [手机版/说明.md](手机版/说明.md) — 在安卓手机上从安装到敲命令
- [本体/docs/桌面系统_架构与路线.md](本体/docs/桌面系统_架构与路线.md) — 图形化架构与分期清单（写于较早阶段，其中「只有 write/exit 两个系统调用」的现状描述已过时）
- `LOONGOS_*_TODO.md` 与 `docs/检查与修复记录*.md` — 各阶段的设计取舍与排障实录

## 许可证

MIT — 见 [LICENSE](LICENSE)。本项目由 NAS 归档包整理后公开发布。
