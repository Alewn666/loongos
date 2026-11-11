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
| 图形桌面 | `gfx.c` / `gfx.h` / `gfx_draw_h` / `desktop.c` | 裁剪 / 命中 / z 序 / 脏矩形纯函数层 + **注册表驱动 App 框架**（多窗口 / 单实例 / 级联开窗 / 拖标题栏 / 点 × 关闭 / 任务栏均分切换）；**开始按钮 = 真正 App 启动器**（点开列出 11 个 App + 关机/重启，点应用即开窗）；内置 **终端 / 安装系统 / 系统信息 / 图形演示 / 操作说明 / 计算器 / 系统监视器 / 设置 / 关于 / 文件管理器 / 文本编辑器**，安装系统向导（二次确认，10 秒超时自动撤销） |
| 桌面手感 P3 | `desktop.c` / `kernel.c` / `gfxterm.c` / `gfx_draw.h` | 去掉主循环 `wfi`（改连续轮询 + 硬件计数器 60fps 帧节流），键鼠输入不再排队等 100ms 拍子；按钮**边沿事件队列**保证极短快速点击 / 短拖拽不丢；**白色闪烁插入光标**（打字后常亮 500ms，之后 500ms 亮/灭） |
| 鼠标校准 | `kernel.c` / `desktop.c` | VNC 客户端送坐标带固定偏移时，`开始菜单 → 鼠标校准` 点靶心自动补偿；`mousecal` 命令查询/手设/清零，偏移存 `/etc/mousecal`，装盘重启仍生效 |
| 图形终端 | `gfxterm.c` | 窗口内可跑**全部** shell 命令（含 `install yes`）；`uart_putc` 输出捕获进 12KB 历史缓冲，中英混排、UTF-8 输入、退格、滚轮翻页、白色闪烁光标 |
| 文件系统 | `fs.c` | 自带 fs，内存盘 / 磁盘持久化 |
| 用户程序 | `kernel.c` / `user/*.c` | ELF 加载与 `run`；`write` / `open` / `read` / `close` / `lseek` / `exit` 等系统调用（`svc #0`）；`hello` `cat` `args` `bad` `hang` 五个样例 |
| 存储 | `virtio_blk.c` / `virtio_blk_legacy.c` / `virtio_scsi.c` / `nvme.c` / `pci.c` | virtio-blk modern/legacy 双路径；**原生 NVMe**（Admin/IO SQ-CQ、PRP、CC/CSTS、Identify），`install yes` 装盘后重启文件仍在；virtio-scsi transitional 因 64 位 BAR 风险主动放弃 |
| 随机数 | `virtio_rng.c` | virtio-rng 设备识别与就绪（UEFI 5 拓扑回归覆盖） |
| 网络 | `virtio_net.c` / `net.c` / `tcp.c` | virtio-net + TCP 协议栈 |
| 加密 | `crypto.c` / `test_crypto.c` | 登录口令校验与哈希 |
| 安装 | `build_iso.sh` / `build_image.sh` | 生成可写入硬盘的 ISO 与磁盘镜像；五方 md5 校验保证 ISO 内嵌内核与源码构建一致 |
| 内核接口 | `loongos.h` | 跨模块声明的唯一事实来源；收敛散落在各 `.c` 的 127 处重复 `extern`，修掉类型漂移与幽灵声明 |
| 块设备抽象 | `blkdev.h` / `blkdev.c` | `struct blkdev_ops` + `blk_drivers[]` 统一优先级表；新增驱动只改 3 处，不再动 `kernel.c` |
| 资源调配 R | `kernel.c` / `desktop.c` / `test_resource_live.py` | 硬件唤醒频率与 `tick`(恒10Hz) 按 cycle 解耦；桌面空闲 `wfi` + 自适应调频（1000/250/120Hz），宿主 CPU **50%→0.6%**；`res` 命令看资源账 |
| 电源管理 | `kernel.c` / `desktop.c` / `test_power.py` | 自研 EL1 内核无 ACPI/guest-agent，收不到虚拟机管理器的电源键；改用 **PSCI** `SYSTEM_OFF(0x84000008)` / `SYSTEM_RESET(0x84000009)` 主动触发，先于 `ID_AA64PFR0_EL1` 探测 conduit（HVC 优先、SMC 兜底、无则优雅返回）；`poweroff`/`shutdown`/`halt`/`reboot` 命令 + 桌面「关机/重启」按钮，等价管理器电源操作 |
| 构建与空间守卫 | `build.sh` / `bss_budget.txt` | `KERN_SRCS` 单一编译单元来源 + 未登记源文件守卫 + 分段预算/上界余量 + fw_cfg DMA 缓冲**段归属 + 地址≥镜像起点**守卫 |
| 真实画面判据 | `test_framebuffer.py` | 直接对 QEMU screendump 做像素级判定（尺寸 800×600 / 颜色数 / 非背景占比），杜绝「串口一切正常但屏幕全黑」的静默假绿 |

## 内核结构优化（2026-11-10）：为后续功能铺路，功能零变化

这一轮**不改任何功能**，只把内核「地基」整理成能继续往上长东西的样子。方法是先固化两份基线，再分四阶段重构，每阶段都用基线证明行为没变：

| 阶段 | 改动 | 「功能不变」的证据 |
| --- | --- | --- |
| 基线 | `golden_behavior.txt`（启动日志 + 29 条命令逐字）+ `kernel.elf` md5 `072bbda9…` | 连续两次采集一致 |
| 一 · 结构解耦 | 新建 `loongos.h` 作跨模块声明唯一来源，删 127 处重复 `extern`；`build.sh` 收敛为 `KERN_SRCS` 单一来源 + 未登记源文件守卫 | 重构后 `kernel.elf` md5 仍为 `072bbda9…`，**逐字节不变** |
| 二 · 块设备抽象 | 新建 `blkdev.h/.c`，virtio-blk / virtio-scsi / NVMe 各导出 `xxx_ops`，`blk_drivers[]` 统一优先级，`vb_*` 保留兼容门面 | 行为基线逐字一致；NVMe 装盘 / UEFI 5 拓扑 / 桌面 / EL0 全通过 |
| 三 · 空间预算 | 定位 BSS 余量 8→4 KB 的根因（`.bss` 内 4 KB 对齐缓冲被一个 4 B 标志顶后一页，是链接对齐碎片、非泄漏）；`build.sh` 增分段预算表 + 三级阈值 + 团队基线 delta | 注入 20 KB 静态缓冲，守卫 fail fast 拦下越界；`kernel.elf` md5 不变 |
| 四 · 交付 | 全套回归 9/9、ISO 重建（内嵌内核五方 md5 一致）、文档与仓库同步 | — |

新增一个块设备驱动（AHCI / USB 存储 / virtio 新类型）现在只需三步：**导出 `xxx_ops` → `blk_drivers[]` 加一项 → `build.sh` 登记 `.c`**，`kernel.c` 和各既有驱动一行都不用改；以前要同时改 5 个门面函数 + 启动探测链 + 类型判断共 7 处，漏一处就「认得到盘但读不了」。

关于上界余量为什么是 4 KB、它会不会悄悄越界、后续真要更多空间怎么扩，见 [内核空间预算与扩容指引](本体/docs/内核空间预算与扩容指引.md)。

## 桌面手感优化 P3（2026-11-10）：像 Windows / 带 GUI 的 Linux

针对真机反馈「鼠标滑动不跟手、点击像点不准、键盘输入有延迟、想要一个闪烁光标」，把桌面从 **10Hz 节拍驱动**改成**输入即时驱动**。三项改动共用同一个根因修复：

| 改动 | 旧行为 | 新行为 |
| --- | --- | --- |
| **主循环去 `wfi`** | 整机只使能 UART(33)/TIMER(30) 两个 IRQ，xHCI 无中断；`wfi` 只能被 10Hz timer 唤醒 → 键鼠事件排队 **0~100ms**，画面 10fps | 连续 `usb_kbd_poll()` 收割，有输入立即 `gfx_present()`；空闲用 `cntpct_el0` 硬件计数器做 **60fps 帧节流**驱动闪烁与时钟；原 10Hz 任务按 `tick` 变化执行，语义不变 |
| **按钮边沿事件队列** | 每轮取「合并按钮状态快照」与上次比较得 pressed/released，一拍内「按下+松开」被吞 → 快速点击、短拖拽偶发失灵 | `mouse_feed_*` 把每次按钮状态变化压入 32 槽环形队列，`mouse_pop_btn()` 逐事件分派，坐标仍读最新值（滑动连续） |
| **白色闪烁插入光标** | 8×16 青绿实心方块、常亮、非白 | 共用绘图层 `gfx_draw_caret()` 画白色方块；打字重置亮相位（500ms 常亮），之后 500ms 亮/灭；相位切换只脏输入行 22px，不重绘 64 行历史 |

**「输入即时化」的量化证据**（`test_desktop_p3.py`，同一套用例跑 P3 前/后内核）：极短快速点击的菜单区域差异从 **0.006（边沿被吞）→ 0.936（菜单打开）**，白色光标闪烁从 **0.000（恒亮）→ 0.267（在闪）**；纯移动跟随与键盘回显两版都 PASS（功能不回归）。键鼠走同一 `usb_kbd_poll()` 收割路径与同一主循环，边沿不丢的结论对键盘同样成立。

> 关于「测延迟」的诚实说明：QMP `input-send-event` / `sendkey` 是异步注入（自带 60ms~数百 ms 抖动），`screendump` 又会在 60fps 局部重绘下抓到半帧，**测试通道测不了内核微秒级延迟**。所以「输入即时化」用确定性的边沿事件判据（T2）证明，不靠「发完立刻截屏」这类不可复现的数字；架构层根因与修复见 `desktop.c` 主循环注释。

闪烁光标是**共用绘图层能力**，同时同步到 X86 分支：`x86/desktop_x86.c` 用 PIT `g_ticks`(100Hz) 驱动同一 `gfx_draw_caret()`，`x86/test_x86_blink.py` 实测相邻帧差异 0.205（在闪），X86 端到端 6/6 全通过。

## 系统资源调配优化 R（2026-11-11）：从「忙等烧一核」到「事件驱动休眠」

P3 把输入延迟修好了，但代价是桌面主循环 `yield` 忙等、**占满一整核**（实测宿主 CPU 50%）。本轮在**不改任何用户可见功能**的前提下，把 CPU、时钟、内存三类资源的调配系统化整理。

### 1. CPU：50.0% → 0.6%（宿主侧 `/proc` jiffies 实测，约 83 倍）

| | 旧（P3 忙等） | 新（R 轮事件驱动） |
| --- | --- | --- |
| 空闲 | 全速轮询 `yield`，占满 CPU0 | 空闲 `wfi` 真正睡眠，被唤醒 IRQ 叫醒即 `poll` |
| 有输入 | 全速 | **仍全速不睡**，P3 跟手感原样保留 |
| 唤醒节奏 | 只有 10Hz timer | 按负载自适应：活跃 1000Hz / 短闲 250Hz / 久静 120Hz |
| 宿主 CPU | **50.0%** | **0.6%**（DEEP 档） |

关键设计：**硬件唤醒频率与软件 `tick` 分频解耦**。`tick` 被 fs mtime、EL0 看门狗、task1 心跳、抢占轮转、桌面时钟/超时写死成 10Hz，不能提频；现在它由 `cntpct_el0` 的真实 cycle 结算（恒 10Hz、自动补拍），硬件 timer 可以独立跑 120~1000Hz 只负责把 CPU 从 `wfi` 叫醒。桌面期间 `g_desktop_active=1` 抑制抢占轮转（否则 `wfi` 醒来会被切到 worker 卡 100ms，正是 P3 当初去掉 `wfi` 的深层原因）。

> 三个实测踩坑（都写进代码注释）：① 用「每 N 次 IRQ 才 tick++」分频在 QEMU TCG 下会让 tick 漂移（设定 1000Hz 实际只触发 ~558Hz）→ 改用 cycle 结算；② `cntpct_el0` 虚拟时间 ≠ 墙钟，空闲计时改用 `tick`；③ 空闲起点只认**真实键鼠事件** `real_input`，若复用被闪烁/时钟污染的 `input` 会永远降不到 DEEP 档。

### 2. 内存：DMA 缓冲段归属守卫 + 「真实画面」判据（含一次重要勘误）

上一轮（P3）把图形启动挂死归因为「fw_cfg DMA 缓冲必须留在 `.data`」，加了段归属守卫。本轮一开始想把地址**钉到固定物理页 `0x4007E000`** 以彻底解耦浮动段链，7 组实验用「串口出现『图形桌面已就绪』/『fwcfg ramfb 已配置』」当成功判据，一度以为成功。

**但这是一次「串口假绿」**：那两句日志是内核无条件打印的，QEMU 的 fw_cfg DMA 其实没建立 ramfb surface（固定地址落在内核加载起点 `0x40080000` 之前的低 RAM / loader-DTB 暂存区），`screendump` 恒为 **640×480 纯黑**。全套图形回归里 P2/P2.1/P2.2 因「画面差异全 0.000」而红，却没有任何一个测试直接说「屏幕是黑的」。用**真实画面**重做 A/B 才看清：静态数组 `.data`（`0x400ae480`）→ 800×600 彩色桌面；固定低地址 → 黑屏。

最终方案（比固定地址约定稳健）：**保持静态数组 `.data`（显式 `={1}` 落 PROGBITS，让 QEMU 加载器实际把该 RAM 页纳入镜像）**，不引入新段/宏；`build.sh` 守卫校验两符号段类型 `d/D` 且绝对地址 ≥ `0x40080000`（镜像内已映射 RAM，禁止退回 `.bss` 或改绝对地址宏）；并新增 `test_framebuffer.py`，**每次回归直接用 screendump 做像素级判定**（尺寸必须 800×600、颜色数 >16、非背景占比 >1%），用行为测试兜底「新增代码后 `.data` 平移是否仍出画面」，而不是靠脆弱的地址约定。`diag_res.py` 可在图形链路异常时读 guest 计数判定「wfi 是真省电还是睡死」。

### 3. 可观测性：`res` 命令 + 在线验证

串口敲 `res` 打印系统资源账（软件 tick / 当前唤醒档位 / 分频 / 桌面态 / TIMER IRQ 累计 / wfi 命中 / 内核上界余量 / SMP 在线核）。刻意**不写进 `help` 文本、不进基线命令集**，保行为基线逐字一致。

`test_resource_live.py` 用 QMP 直接读 guest 内存，在线验证：桌面态=1、TIMER IRQ 与 wfi 命中持续增长（证明**省电不是靠冻住画面**）、自适应降到 DEEP 120Hz、tick 恒 10Hz（3.2s 精确 +32）——全部 ALL PASS。`test_idle_cpu.py` 用宿主 `/proc/<qemu-pid>/task/*/stat` 的 jiffies 测 CPU 时间，guest 无法伪造。

> 诚实边界：CPU 数字来自 QEMU TCG 沙箱，真机 KVM 下 `wfi`/timer 语义更接近硬件，降幅方向一致但绝对值不同；xHCI 仍是纯轮询（未加 MSI 中断），高频唤醒是为此付出的固定小成本；本轮未做内存池 / 动态分配 / SMP 主逻辑分工，静态预算模型不变。

## 从源码到开机

构建脚本做的事很直接：先把用户程序编成独立 ELF，用 `objcopy` 嵌成 blob，再和内核各编译单元一起链成 `kernel.elf`。

```bash
cd 本体
sh build.sh                # 需要 aarch64-linux-gnu- 工具链；内含内核空间预算守卫
                           # （每次构建打印各段大小与上界余量，见 docs/内核空间预算与扩容指引.md）
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
>
> ⚠️ **旧 ISO 会导致装不上**：早期版本只扫 bus0 上的一级桥，日志特征是 `扫描总线数=2 / 没找到 virtio-scsi 设备`；新版已改为多级桥 BFS（真机同构拓扑实测 `扫描总线数=5`、识别桥后 bus4 的控制器）。如果 VM 的 CD-ROM 还指向旧的 `loongos-kvmfix.iso`，请改成本页的 `loongos.iso`——QEMU 已打开的文件句柄即使文件被删仍有效，会让人误以为「已经在跑新系统」。

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
                                                                （P3：按钮边沿事件队列 + 连续轮询）
HAL     framebuffer(GOP/ramfb) │ USB xHCI │ 10Hz timer │ cntpct_el0 60fps 帧节流
```

- **P1 ✅**：渐变壁纸 + 顶栏 + 任务栏时钟、中英混排、脏矩形、开机默认进桌面、无 GOP 优雅降级。
- **P2 ✅**：三设备同挂枚举、鼠标指针、点击 / 拖拽 / 关闭 / z 序、开始菜单 11 项、安装向导、图形终端跑全部命令、退出键改为**仅 Ctrl+Shift+F1**。
- **P2.1 ✅**：鼠标固定偏移校准（点靶心自动补偿 + `mousecal` 命令 + 持久化）、virtio-scsi 放宽 ID 与失败自诊断。~~绝对/相对坐标源互斥~~（该做法已在 P2.2 撤销）。
- **P2.2 ✅**：修复 P2.1 引入的「相对鼠标不跟随」回归——去掉基于 `tick` 的 2 秒时间窗口，相对增量一律累加、纯按钮报告不动坐标；新增 `test_mousemove.py` 覆盖「只移动不点击」盲区；`mousecal` 增加绝对/相对输入源统计。
- **P3 ✅**：桌面手感全面优化（详见上方专章）——主循环去 `wfi` 改连续轮询 + 60fps 帧节流、按钮边沿事件队列、白色闪烁插入光标；新增 `test_desktop_p3.py` 做 P3 前/后量化对照，共用绘图层能力同步到 X86（`x86/test_x86_blink.py`）。
- **P3+ ✅（第三十四轮）**：开始按钮改造为真正的 **App 启动器**（`menu_build()` 动态生成 App 列表，点应用 `open_app()` 开窗）；落地完整 **GUI 应用生态**——注册表驱动 App 框架 + 11 个 App（终端 / 安装系统 / 系统信息 / 图形演示 / 操作说明 / 计算器 / 系统监视器 / 设置 / 关于 / 文件管理器 / 文本编辑器），文件管理器直连 `fs.h` 浏览目录、文本编辑器可编辑保存；新增 `test_launcher.py` 用 QMP 注入真实点击验证「点开始→菜单展开→点应用→窗口绘制」全链路。待做：窗口缩放与最小化、右键菜单、更换字库（见「诚实的边界」）。

实测截图见 [docs/screenshots/](本体/docs/screenshots/)，逐轮设计与排障记录见 `LOONGOS_DESKTOP_TODO.md` 与 `docs/检查与修复记录.md`。

## 测试是怎么推进的

这个内核按阶段加能力，测试也按阶段留了下来，改任何一处都能回归：

```bash
cd 本体
# 「功能不变」的两道基线（内核结构性优化后必跑）
python3 test_behavior.py --compare golden_behavior.txt   # 启动日志 + 29 条命令逐字比对
                                                        # build.sh 末尾自动打印内核空间预算表
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
python3 test_mousecal.py           # 鼠标校准：串口命令/持久化 + 图形点靶心对齐
python3 test_mousemove.py          # 只移动不点击：绝对/相对坐标下光标实时跟随（P2.2）
python3 test_desktop_p3.py         # P3 手感验收：快速点击边沿 / 白色闪烁光标 /
                                   # 纯移动跟随 / 键盘回显（可指定 KERNEL= 跑 P3 前后对照）
bash test_drivers.sh               # 直启 TCG 抓设备识别
cd ../x86 && python3 test_x86.py           # X86 端到端 6/6（Linux 风格输入 + 桌面）
cd ../x86 && python3 test_x86_blink.py     # X86 白色闪烁光标专项（共用绘图层）
python3 test_idle_cpu.py           # R 轮：宿主侧 /proc jiffies 测桌面空闲 CPU（50%->0.6%）
python3 test_resource_live.py      # R 轮：QMP 读 guest 内存验证 wfi/tick/自适应调频 ALL PASS
```

另外还有 `bench_boot.py` 启动耗时基准，以及 `diag_serial.py`、`probe_mmio.py`、`probe_pte.py`、`probe_drag.py` 等一批排障脚本——写内核时踩过的坑基本都留了可复现的工具。测试脚本均用 `cd "$(dirname "$0")"`，clone 到任意目录都能跑。

## 目录结构

```
loongos/
├── 本体/                 # 内核源码、构建与测试（主体）
│   ├── boot.S kernel.c smp.c fs.c crypto.c net.c tcp.c
│   ├── loongos.h         # 跨模块声明唯一事实来源（2026-11-10 内核优化）
│   ├── blkdev.h blkdev.c # 块设备统一抽象层：驱动表 + 统一门面
│   ├── usb_kbd.c virtio_*.c nvme.c pci.c virtio_rng.c uefi_stub.c
│   ├── gfx.h gfx.c gfx_draw.h desktop.c desktop.h gfxterm.c gfxterm.h
│   ├── font8x16.h font_cjk.h linker.ld auth_cred.h
│   ├── user/             # 用户态 ELF 样例程序
│   ├── build.sh build_iso.sh build_image.sh build_pe.py
│   ├── bss_budget.txt    # 内核空间预算基线（build.sh 自动维护，纳入版本库）
│   ├── golden_behavior.txt  # 行为黄金基线：启动日志 + 29 条命令
│   ├── run.sh run_full.sh make_disk.sh   # 最简 / 满配启动 / 磁盘镜像
│   ├── test_stage*.py test_desktop_p2.py test_nvme_install.py
│   ├── test_behavior.py  # 行为基线采集/逐字比对
│   ├── test_idle_cpu.py test_resource_live.py  # R 轮资源调配量化/在线验证
│   ├── test_uefi.sh test_drivers.sh test_wm_host.c test_net_host.c
│   ├── LOONGOS_*_TODO.md  # 各阶段任务清单（内核演进档案）
│   └── docs/             # 桌面架构、检查与修复记录、内核空间预算与扩容指引、实测截图
└── 手机版/               # Termux + QEMU 运行说明与修复记录
```

## 诚实的边界

- 用户态目前提供 `write` / `open` / `read` / `close` / `lseek` / `exit` 等系统调用，还没有堆分配器，内存全是静态 BSS；EL0 有 30 秒看门狗防止用户程序卡死内核。桌面与窗口同样是**内核态静态结构**，应用不是独立进程，一个窗口崩溃会影响整个系统。
- **多核 SMP 默认关闭，不是「已支持多核」**：QEMU 加 `-machine virt,virtualization=on` 时 PSCI 能拉起 `cpu1` 并在线，但从核定时期中断进不了处理程序，`wfi` 会满速空转烧核（实测 2.5 秒涨 5000 万+ 次）。本轮已修好从核向量表的 128B 槽距（原 IRQ 槽错落在 `+0x14` 而非 `0x280`）与 PSCI conduit 探测（无 EL3/EL2 时优雅跳过，不再刷异常转储）；剩下的是 GIC CPU interface 打通。`-DSMP_ENABLE` 仅供调试。
- **磁盘别用 `virtio-scsi-pci` 测**：驱动对 transitional 设备主动放弃（真机 PCIe 上改写 64 位 BAR 会 external abort，代码里有 2026-10-25 实测记录）。QEMU 用 `-device virtio-blk-pci` 即走 virtio-blk 路径，8MB 盘能读写、能格式化、能持久挂载；NVMe 盘走 `nvme.c` 原生驱动，装盘与持久化已通过端到端验证。
- **AAVMF 没有 NVMe 引导驱动**：NVMe 盘只能由内核起来后识别，不能作为 UEFI 启动盘；安装仍从 ISO/光盘引导。
- **字库有已知缺陷**：`font_cjk.h` 里 `·`(U+00B7) 字形本身是两条竖线，部分框形字（口/回/窗）缺底横——是 885 个字形的历史数据问题，尚未整体更换；桌面文案已避开这些字。
- **桌面与鼠标只在 TCG 下实测**：沙箱无 `/dev/kvm`，`test_desktop_p2.py` 8/8 全在 QEMU TCG 完成；真机是 KVM，装机后建议确认一次「进桌面 → 鼠标拖拽 → Ctrl+Shift+F1 → install/net」。
- 输入是**事件驱动 + 连续轮询的混合**（R 轮）：xHCI 无 IRQ，键鼠仍靠 `usb_kbd_poll()` 轮询收割，但 R 轮起桌面空闲会 `wfi` 睡眠、由 120~1000Hz 自适应唤醒 IRQ 叫醒（有输入时全速不睡），宿主 CPU 从 50% 降到 0.6%；没有触摸 / 多点触控 / 手势；窗口缩放、最小化、右键菜单仍在后续清单里。xHCI 未接 MSI 中断，高频唤醒是为「纯轮询设备要低延迟」付出的固定小成本；CPU 数字来自 QEMU TCG 沙箱，真机 KVM 下绝对值会不同。
- **网页 VNC 的鼠标固定偏移**来自客户端画布留白 / 缩放没扣（VNC 走相对模式时，内核光标从屏幕中心累加位移，与客户端物理光标起点本就不同），内核改不了 VNC：`开始菜单 → 鼠标校准` 点一次靶心即可补偿，`mousecal` 命令可查询 / 手设 / 清零，并打印绝对/相对报告数判断 VNC 走哪种模式。偏移存 `/etc/mousecal`，未装盘（内存盘）时重启会丢。P2.2 已修复「相对位移被吞、鼠标不跟随」的回归。
- 目标平台是 QEMU `virt` 与标准 UEFI aarch64，未适配具体实体 ARM 板卡的板级驱动。
- 仓库里的 `BOOTAA64.EFI`、`kernel.elf` 与若干 `.o` 是构建产物，便于直接体验；源码以 `build.sh` 为准，新增编译产物不再纳入版本控制（见 `.gitignore`）。

## 文档索引

- [手机版/说明.md](手机版/说明.md) — 在安卓手机上从安装到敲命令
- [本体/LOONGOS_DESKTOP_TODO.md](本体/LOONGOS_DESKTOP_TODO.md) — 桌面 P1/P2 验收清单与 P3+ 待办
- [本体/docs/桌面系统_架构与路线.md](本体/docs/桌面系统_架构与路线.md) — 早期图形化架构与分期清单（其中「只有 write/exit 两个系统调用」的现状描述已过时）
- [本体/docs/内核空间预算与扩容指引.md](本体/docs/内核空间预算与扩容指引.md) — 内核链接布局、BSS 余量 ±4 KB 对齐碎片根因、预算守卫与扩容路径
- [本体/docs/系统资源调配.md](本体/docs/系统资源调配.md) — R 轮：CPU/时钟/内存三类资源的调配设计、7 组 DMA 地址对照实验、`res` 资源账与量化验收
- [本体/docs/检查与修复记录.md](本体/docs/检查与修复记录.md) — 逐轮设计取舍与排障实录（第三十一轮 = R 轮）
- `LOONGOS_*_TODO.md` 与 `docs/检查与修复记录*.md` — 各阶段任务清单

## 许可证

MIT — 见 [LICENSE](LICENSE)。本项目由 NAS 归档包整理后公开发布。
