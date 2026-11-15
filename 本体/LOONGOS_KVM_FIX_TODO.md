# LOONGOS_KVM_FIX — 修飞牛虚拟机(KVM) 上 shell 提示符处内核异常
- [x] 1. 验证 vectors 链接地址 2KB 对齐（nm kernel.elf）——修前 0x4008027c FAIL，修后 0x40080800 PASS
- [x] 2. NAS KVM 复现（-enable-kvm + AAVMF + ISO），-d int 抓证据——真因：ISO 内是旧内核（boot.S 19:14 加 .balign 2048 但 kernel.elf 19:03 就编好了）；KVM+GICv3 冒烟修后 12/12
- [x] 3. 定位真因并修——VBAR 错位（老签名 ESR=0xdbadc0de）+ 顺带抓到既有 bug：USER_BASE 上挪 0x401D0000 后 build.sh 用户程序还链在 0x401C1000 → run 拒跑
- [ ] 4. TCG + KVM 双路回归 + 重打 ISO + NAS 归档 md5 对账
- [ ] 5. 留痕写入 docs/检查与修复记录.md

> 第五十轮（2026-11-15）状态注记：4/5 仍**未销账**——真机 KVM 路未执行。
> 本轮已备好 `test_kvm_dual.py`（一键跑 TCG+KVM 双路、自动判 /dev/kvm）、
> 跑了 TCG 路 14/14 对照基线、完成 47/48/49 轮改动的 KVM 敏感点静态审查与 NAS 能力探测；
> 留痕见 `docs/检查与修复记录.md` 第五十轮。剩余唯一动作是在 NAS 侧以有 `/dev/kvm`
> 权限的账户执行脚本的 KVM 路（或飞牛 VM 图形验收）——当前 SSH 账户 `Alewn` 无该权限，
> 详见第五十轮第六节操作清单。KVM 路跑通并拿到对照数字后才可勾选 4/5。
