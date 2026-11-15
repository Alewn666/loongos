# LOONGOS_HB_FIX — 后台心跳刷屏：只走串口不画屏幕
- [x] 1. 定位：task1_main 每 10 片（约 2 秒）打一行「我在干活」，uart_putc 会镜像到 GOP 帧缓冲 → 真机屏幕刷屏
- [x] 2. 修法：新增 hb_putc/hb_puts/hb_puthex（只写 UART_DR，不经过 fb_putc），task1 心跳改用 hb_*；串口保留心跳（测试/调试要用）
- [x] 3. 重编 + 重打 ISO
- [x] 4. 全套回归 + TCG/KVM ISO 冒烟
- [x] 5. NAS 归档 + 留痕

> 第五十一轮（2026-11-15）**4/5 已销账**：全套回归（TCG 15/15）第四十九轮已完成；
> 双路里的 KVM 一半本轮在飞牛 NAS aarch64 真机以 root 跑通——`test_kvm_dual.py`
> **TCG 14/14 + KVM 14/14**（`-cpu host -enable-kvm -gicv3`，健康计数全 0，
> KDF 真机加速约 5.4×）。第 49 轮 ISO md5 `0a3af869…` 已在 NAS 归档对账、与本地一致。
> 留痕见 `docs/检查与修复记录.md` 第五十一轮。root 通道真相：第五十轮误把免密 sudo
> 失败 + sshd `MaxStartups` 限流当成「无权限」，实际 `Alewn` + `sudo -S` 即可提权。
> 第五十二轮（2026-11-15）**图形侧自动复验已补上**：`本体/test_kvm_graphic.py` 在同一台
> 飞牛 NAS aarch64 真机以 root 起独立临时 QEMU（AAVMF + ISO + ramfb 800×600 + 三 HID），
> 用 QMP `screendump` 真实像素 + 注入点击/拖拽 + HMP `sendkey` 跑通
> **TCG 15/15 + KVM 15/15**；开机进桌面、菜单、窗口拖动、Ctrl+Shift+F1、install（未发
> `yes`）、net、bench 图形态、res 健康全 0 全过，截图与报告见 `docs/screenshots/` 与
> `docs/真机KVM_图形复验报告.json`。本轮不启停/不改动用户 VM。
> 仍未做的只有「人工在 Web VNC 里拖拽的体感验收」与触屏/手势真机专项——
> QMP 注入模拟 HID 不等价于人工操作，不冒充已完成。见第五十二轮留痕。
