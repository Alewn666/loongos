# LOONGOS_KVM_FIX — 修飞牛虚拟机(KVM) 上 shell 提示符处内核异常
- [x] 1. 验证 vectors 链接地址 2KB 对齐（nm kernel.elf）——修前 0x4008027c FAIL，修后 0x40080800 PASS
- [x] 2. NAS KVM 复现（-enable-kvm + AAVMF + ISO），-d int 抓证据——真因：ISO 内是旧内核（boot.S 19:14 加 .balign 2048 但 kernel.elf 19:03 就编好了）；KVM+GICv3 冒烟修后 12/12
- [x] 3. 定位真因并修——VBAR 错位（老签名 ESR=0xdbadc0de）+ 顺带抓到既有 bug：USER_BASE 上挪 0x401D0000 后 build.sh 用户程序还链在 0x401C1000 → run 拒跑
- [x] 4. TCG + KVM 双路回归 + 重打 ISO + NAS 归档 md5 对账
- [x] 5. 留痕写入 docs/检查与修复记录.md

> 第五十一轮（2026-11-15）**4/5 已销账**：查明 root 权限一直可用（`Alewn` + `sudo -S`
> 提权，第五十轮把免密 sudo 失败 + sshd `MaxStartups` 限流误读成「无权限」），本会话
> 直接在飞牛 NAS aarch64 真机以 root 跑通 `test_kvm_dual.py`：**TCG 14/14 + KVM 14/14**
> （`-cpu host -enable-kvm -gicv3`，健康计数全 0，KDF 真机加速约 5.4×）。第 49 轮 ISO
> md5 `0a3af869…` 与本地定稿一致、已在 NAS 归档对账；本轮零内核源码改动故未重打 ISO。
> 留痕见 `docs/检查与修复记录.md` 第五十一轮（含对第五十轮结论的更正与诚实边界）。
> 第五十二轮（2026-11-15）**路线 B 的自动判据部分已销账**：新增 `本体/test_kvm_graphic.py`，
> 在同一台飞牛 NAS aarch64 真机以 root 起独立临时 QEMU（AAVMF + 第 49 轮 ISO + ramfb
> 800×600 + qemu-xhci 三 HID），用 QMP `screendump` 真实像素 + `input-send-event` 注入
> 点击/拖拽 + HMP `sendkey`，**TCG 15/15 + KVM 15/15**：进桌面 / 开始菜单 / 应用窗口 /
> 标题栏拖动 / Ctrl+Shift+F1 / 退出后键盘 / install（未发 yes）/ net / bench 图形态 /
> res 健康全 0 全过；截图 `docs/screenshots/真机KVM_图形交互总览.png`、报告
> `docs/真机KVM_图形复验报告.json`。**不启停/不改动任何用户 libvirt VM**。
> 仍未做的只有「用户在 Web VNC 里手工拖拽的体感验收」——QMP 注入是模拟 HID，
> 不等价于人工操作；触屏/手势、窗口缩放在真机的专项复验也仍单列保留。
> 留痕见 `docs/检查与修复记录.md` 第五十二轮。
