# LOONGOS_DESKTOP — 把 LoongOS 做成有窗口/鼠标/应用的图形系统

> 完整架构与路线见 `docs/桌面系统_架构与路线.md`

## P1 · 图形地基 ✅（2026-11-08 完成）
> **用户拍板**：「进系统默认直接进桌面」。已改为**开机默认进图形桌面**，
> 串口 shell 保留为调试/回归通道（串口一有输入桌面自动让位，字符不吞）。
> ⚠️ 退出快捷键在 P2 按用户要求改为 **Ctrl+Shift+F1**（P1 的 `Q`/`Esc` 已取消），
> 终端敲 `desktop` 可再次进入。
- [x] `gfx.h` 定接口（裁剪栈 / fill / rect / line / text / damage）
- [x] `gfx.c` 实现：bootinfo 取 fb 参数、32bpp 探测、裁剪、中英混排文字
      （新增 `gfx_draw.h` 分离「纯函数层」与「有状态绘图层」；字库走 `fb_query_glyph()` 不复制 32KB）
- [x] `desktop.c`：壁纸渐变 + 顶栏 + 任务栏（时钟）+ **3 个可操作窗口**（W/A/S/D 移动、N/Tab 切换、1/2/3 选中）
- [x] `kernel.c` main 接入 `desktop_run()`，**默认调用**；ring 加来源标记实现输入分流；
      `uart_putc` 增加「屏幕所有权」闸（桌面接管期间日志不冲画面）
- [x] `build.sh` 加编译单元（`gfx.o` / `desktop.o` 进 ld）
- [x] `test_wm_host.c` 宿主单测 **36/36 通过**（裁剪求交 / 窗口命中 / z 序）
- [x] 构建零警告、四项 BSS 守卫通过（余量 36 KB）
- [x] QEMU/UEFI + ramfb 实测：开机默认进桌面、screendump 抓图确认画面、
      USB 键盘 W 移动窗口 / Q 返回终端、串口 `help` 自动让位、`desktop` 再次进入
- [x] 全套回归通过：5 拓扑 + NVMe 装盘 ALL PASS + `test_stage5b_elf.py` 33/33

**已知遗留（不属 P1 范围）**：`font_cjk.h` 的 `·`(U+00B7) 字形本身是两条竖线、
「口/回/窗」等框形字缺底横——是历史字库生成器的产物，串口终端同款，非桌面引入；
字体源与 `tools/gen_font.py` 已不在仓库，留待 P4 换字库时一并处理。

## P2 · 鼠标 + 图形化操作全部功能 ✅（2026-11-08 完成）
> **用户拍板**：「这个页面我挺喜欢的。为它加上可以用鼠标操作和可以在图形页面上
> 操作电脑的所有功能，包括安装系统，而且要退出桌面系统需要按下 Ctrl Shift 加 F1」。
- [x] `usb_kbd.c` 重构为**多设备 HID 驱动**：单 slot → `MAX_HID=4` 个 slot，
      各自 dev_ctx / ep0_ring / 中断环 / 报告缓冲；一个事件环按 `(slot,dci)` 反查设备
- [x] 识别三类 HID：boot keyboard(proto=1, 8B)、**相对鼠标**(proto=2, 4B)、
      **绝对平板**(proto=0, 6B 绝对坐标 0..32767)；真机 VM 三设备同挂一个 qemu-xhci 全部枚举成功
- [x] `kernel.c` 鼠标桥：`mouse_feed_abs/rel` → `mouse_get` 快照；坐标映射 + 裁剪；
      **按钮按来源分别保存再按位或合并**（修 tablet+mouse 共存时按下位被覆盖的 bug）
- [x] 光标绘制（脏矩形局部重画，只脏旧/新两个小矩形）
- [x] 窗口交互：点击提焦点、拖标题栏移动、点 × 关闭、任务栏切换、z 序提升
- [x] **开始菜单** 10 项：图形终端 / 安装系统 / 系统信息 / 操作说明 /
      命令清单(help) / 查看磁盘(disks) / 连接网络(net dhcp) / 网络状态(ifconfig) /
      磁盘用量(df) / 清空终端
- [x] **安装系统向导**：二次确认（10 秒超时自动撤销）→ `install yes`，
      先 `disks` 看目标盘再装，输出显示在图形终端里
- [x] **`gfxterm.c` 图形终端**：输入行编辑器（UTF-8/退格/回车）、历史缓冲、
      中英混排渲染、滚轮翻页、可点击快捷命令；
      `uart_putc` 挂钩把命令输出捕获进窗口 → **全部 shell 命令都能在图形界面跑**
      （`help` / `ls` / `net` / `install yes` / 管道 / 重定向 …）
- [x] **退出键改为仅 Ctrl+Shift+F1**：HID 报告层识别 modifier(LCtrl/LShift/RCtrl/RShift)
      + F1(usage 0x3A)，置 `desktop_exit_requested()` 标志；Q/Esc 不再退出；串口输入仍自动让位
- [x] `build.sh` 加 `gfxterm.o`；编译零警告，四项 BSS 守卫通过（余量 12 KB）
- [x] `test_desktop_p2.py` **8/8 ALL PASS**：三设备枚举、开机进桌面、screendump 抓图、
      鼠标点开始菜单、点菜单跑 help、拖标题栏移动窗口、Ctrl+Shift+F1 返回终端、退出后键盘仍可操作 shell
- [x] 全套回归：`test_wm_host.c` 36/36；UEFI 5 拓扑 / NVMe 装盘 / ELF 33/33（无 GOP 时优雅降级）

## P3 · 首批应用
- [x] 终端窗口（把 shell 搬进窗口，P2 已随 gfxterm 完成，且支持全部命令）
- [ ] 文件管理器（复用 fs.c，图形化 ls/cd/cat/write）
- [ ] 文本编辑器（打开/编辑/保存）

## P4 · 打磨
- [ ] 窗口缩放 + 最小化到任务栏
- [ ] 右键菜单 / 双击打开
- [ ] 贪吃蛇
- [ ] 圆角 / 阴影 / 换字库（修 `·` 与框形字缺底横）
- [x] 壁纸渐变（P1 已实现）

## P5 · 可选：应用进程化
- [ ] EL0 syscall：FB_MAP / FB_FLUSH / EVENT_WAIT
- [ ] 每应用独立地址空间，崩溃不拖垮桌面

## 真机拓扑（已验证无需用户改动）
- 用户 VM 原本就有 `qemu-xhci` + `tablet(port1)` + `mouse(port2)` + `keyboard(port3)`，
  P2 的多设备驱动已按此拓扑实测通过，**不需要再手动加设备**。
