# LOONGOS_DESKTOP — 把 LoongOS 做成有窗口/鼠标/应用的图形系统

> 完整架构与路线见 `docs/桌面系统_架构与路线.md`

## P1 · 图形地基
- [ ] `gfx.h` 定接口（裁剪栈 / fill / rect / line / text / damage）
- [ ] `gfx.c` 实现：bootinfo 取 fb 参数、32bpp 探测、裁剪、中英混排文字
- [ ] `desktop.c`：壁纸 + 任务栏（时钟）+ 一个测试窗口
- [ ] `kernel.c` main 接入 `desktop_run()`，保留串口 shell 作调试通道
- [ ] `build.sh` 加编译单元（新增 .c 必须进脚本）
- [ ] `test_wm_host.c` 宿主单测（裁剪求交 / 窗口命中 / z 序）
- [ ] 构建零警告 + `nm kernel.elf | grep vectors` 2KB 对齐自检

## P2 · 鼠标 + 窗口交互
- [ ] `usb_mouse.c`（HID boot mouse 相对坐标）
- [ ] `usb-tablet` 绝对坐标支持（优先）
- [ ] 光标绘制（保存/恢复背景）
- [ ] 窗口：聚焦、拖标题栏、关闭、z 序提升

## P3 · 首批应用
- [ ] 文件管理器（复用 fs.c）
- [ ] 文本编辑器（打开/编辑/保存）
- [ ] 终端窗口（把 shell 搬进窗口）

## P4 · 打磨
- [ ] 窗口缩放 + 最小化到任务栏
- [ ] 右键菜单 / 双击打开
- [ ] 贪吃蛇
- [ ] 壁纸渐变 / 圆角 / 阴影

## P5 · 可选：应用进程化
- [ ] EL0 syscall：FB_MAP / FB_FLUSH / EVENT_WAIT
- [ ] 每应用独立地址空间，崩溃不拖垮桌面

## 待用户配合
- [ ] 用户 VM 增加 USB 鼠标设备（`-device usb-tablet` 或 libvirt `<input type='tablet' bus='usb'/>`）
