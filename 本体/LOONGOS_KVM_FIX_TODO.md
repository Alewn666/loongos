# LOONGOS_KVM_FIX — 修飞牛虚拟机(KVM) 上 shell 提示符处内核异常
- [x] 1. 验证 vectors 链接地址 2KB 对齐（nm kernel.elf）——修前 0x4008027c FAIL，修后 0x40080800 PASS
- [x] 2. NAS KVM 复现（-enable-kvm + AAVMF + ISO），-d int 抓证据——真因：ISO 内是旧内核（boot.S 19:14 加 .balign 2048 但 kernel.elf 19:03 就编好了）；KVM+GICv3 冒烟修后 12/12
- [x] 3. 定位真因并修——VBAR 错位（老签名 ESR=0xdbadc0de）+ 顺带抓到既有 bug：USER_BASE 上挪 0x401D0000 后 build.sh 用户程序还链在 0x401C1000 → run 拒跑
- [ ] 4. TCG + KVM 双路回归 + 重打 ISO + NAS 归档 md5 对账
- [ ] 5. 留痕写入 docs/检查与修复记录.md
