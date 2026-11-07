# LOONGOS_FIXES — 全量代码审查 17 项修复（2026-10-28，用户「先用稳定可靠的方法解决掉这些问题」）
# 状态：全部完成 ✅（主机端验证全过；需用户 VM 实测的项已列入留痕文档）
# 留痕：loongos/docs/检查与修复记录.md 第二十一轮

- [x] 1 split_path name[64] 栈溢出：先卡长度（>61 拒）再拷贝 —— test_fs_host 30/30
- [x] 2 EL0 spsr 0x3c0→0x340（开 IRQ，死循环不再整机挂死）
- [x] 3 EL0 看门狗：30 秒无系统调用强杀回 el1_landing（tick 快照 + handle_irq 插手）
- [x] 4 fs_unlink 顺序：先抹目录项落盘、后 ifree
- [x] 5 fs_format_now 位图清零（磁盘重格式化不再带旧占用；主机端不可测，代码评审+VM 实测）
- [x] 6 fs_setfile 重写：先写新块→iput→后放旧块；失败回滚 —— 盘满回滚测试过
- [x] 7 ELF：filesz>memsz 拒跑 + offset 回绕防护
- [x] 8 口令去明文：auth_cred.h 预计算哈希（盐/迭代单一来源）
- [x] 9 gen_authhash.c 宿主工具（生成 + -c 自检；明文只在此文件）
- [x] 10 AUTH_ITER 2000→50000；auth_init 用预计算哈希；横幅不再打印明文口令（strings 命中 0）
- [x] 11 virtio_scsi else 分支 vs_desc→别名 D
- [x] 12 ESP 分区类型 0x0C→0xEF
- [x] 13 cmd_install 文案与判据一致（数据区 ≥4MB + 打印全盘 MB）
- [x] 14 cmd_df 按介质显示 磁盘/RAM 盘
- [x] 15 el0_page 与 EL0 栈分离（新增 el0_stack[4096] + 映射）
- [x] 16 删 virtio_scsi 死代码（bar_read/bar_size/bar_assign + 未用 cdb）→ 零警告编译
- [x] 17 陈旧注释订正（linker.ld 32KB 栈 / crypto.c / MyOS→LoongOS / seed_one；-append 全仓复查本就不存在）

## 验证（全部可重跑）
- [x] 零警告零错误：`rm -f *.o kernel.elf && sh build.sh 2>&1 | grep -icE "warning|error"` → 0
- [x] fs 主机测试 test_fs_host.c → 30/30；crypto KAT → 9/9；gen_authhash -c → OK
- [x] strings kernel.elf 明文口令命中 0；ISO9660 树 / FAT ESP / 刚编 kernel.elf 三方 md5 一致
- [ ] 用户 VM 实测：run 死循环 30s 被强杀 / su 登录无感延迟 / shadow :50000 / df 磁盘字样 / install 0xEF

产物：kernel.elf fbb3ea1c…、iso/loongos.iso 5d170a75…；回滚点 /tmp/*.bak_r28 + apply_loongos_fixes.py
