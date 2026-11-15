# LOONGOS_HB_FIX — 后台心跳刷屏：只走串口不画屏幕
- [x] 1. 定位：task1_main 每 10 片（约 2 秒）打一行「我在干活」，uart_putc 会镜像到 GOP 帧缓冲 → 真机屏幕刷屏
- [x] 2. 修法：新增 hb_putc/hb_puts/hb_puthex（只写 UART_DR，不经过 fb_putc），task1 心跳改用 hb_*；串口保留心跳（测试/调试要用）
- [x] 3. 重编 + 重打 ISO
- [ ] 4. 全套回归 + TCG/KVM ISO 冒烟
- [ ] 5. NAS 归档 + 留痕

> 第五十轮（2026-11-15）状态注记：4/5 仍**未销账**——「TCG/KVM ISO 冒烟」的 KVM 路未执行。
> 注：全套回归（TCG 15/15）早已在第四十九轮完成并留痕，此处挂的是双路里的 KVM 那一半。
> 本轮已备好 `test_kvm_dual.py` 并跑通 TCG 路 14/14，静态审查 + NAS 能力探测见
> `docs/检查与修复记录.md` 第五十轮。剩余动作＝在 NAS 侧以有 `/dev/kvm` 权限账户执行
> KVM 路（第五十轮第六节清单）；当前 SSH 账户无该权限，跑通后才可勾选。
