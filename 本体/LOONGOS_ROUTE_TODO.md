# LOONGOS_ROUTE — 演进路线（对齐 Linux/BSD）轮次任务清单

> 依据：用户附件《LoongOS 演进路线：对齐 Linux/BSD 操作体验 + 差异化特性》v1.0
> 逐项状态副本见 `本体/docs/演进路线_逐项状态.md`（**该文件是本清单的事实来源**）。

## 第 54 轮：地基（本轮，2026-11-15）

- [x] 2.1 物理页分配器 + `kmalloc`：`mm.c/h`、8 档 size-class、`kmalloc_test` 1000×2 轮零泄漏、`[heap]` 构建守卫
- [x] 2.2 真实墙钟：`rtc.c/h` PL031 + 纯整数历法 + `date`/`date -u`/`date tz`、`ls -l` 真实时间戳
- [x] 增量小项：`sysctl`/`uptime`/`free`/`version`（命令式，刻意不进 help/基线）
- [x] 3.3 口令常量时间比较：`ct_memeq()` + `test_crypto.c` 16/16
- [x] 行为基线 `golden_behavior.txt` 逐字不变；`uptime` 补秒数格式
- [x] 宿主单测：mm/rtc 37/37、fs 30/30、crypto 16/16、gfx 36/36、net 52/52
- [x] `build.sh` BUILD_RC=0、零警告、四项守卫 + heap 守卫、上界余量 32KB
- [x] 重打 ISO + 更新 `.md5`
- [x] 新增双路判据 `test_round54.py`：本地 TCG 22/22
- [x] 全套 15 项回归（结果见 `docs/检查与修复记录.md` 第五十四轮）
- [ ] **真机 NAS KVM 双路复验**：本轮 NAS SSH 通道 14 次探测不可达，KVM 路未执行，
      待通道恢复后以 root 跑 `R54_ISO=… python3 test_round54.py` 补齐（不冒充双路完成）
- [ ] 留痕、GitHub push、NAS sync/verify

### 第 54 轮遗留偏差（登记为技术债，不销账）

- ⚠️ **2.3 shell 迁出内核未做**（依赖第 55 轮 syscall 扩容）。
- ⚠️ **2.2 验收②未满足**：`stat` 仍输出开机秒数（保 29 条基线），真实墙钟在 `ls -l`；
  且 ISO 内存盘重启不持久。需在第 57 轮持久化盘 + 改 `stat` 语义时一并收口。
- ⚠️ **违背「不在 `kernel.c` 继续加 `cmd_*`」不做清单**：本轮新增 7 个 `cmd_*`。
  第 55 轮做用户态 shell 时把这 7 个命令一并迁出 `kernel.c`。
- ⚠️ **`vmalloc`（连续页 + 映射 + 内核栈）未做**，随第 57 轮进程模型一起。

## 第 55 轮：syscall 扩容 + 用户态 shell（2026-11-16 完成 syscall 这一半）

- [x] stat/fstat/dup/dup2/pipe/getpid/mkdir/unlink/rename/kill/getcwd（11 个点名全部 + chdir/getdents/creat 共 14 号 6~19）
- [x] `usysfd.c/h` 统一 fd/pipe 对象层，ring 用第 54 轮 kmalloc，`test_usysfd_host.c` 57/57
- [x] `user/u55_systest.c`（19 号 syscall 冒烟）、`user/u55_sh.c`（`ls|grep|wc` 真 pipe+dup2），经 b64 注入 /home
- [x] `golden_behavior.txt` 29 条命令逐字一致（0~5 号老 syscall 零扰动，`test_stage5b_elf.py` 33/33）
- [x] `test_round55.py` 本地 TCG 25/25；全套回归 15/15
- [ ] **`sh` 作为 EL0 交互 shell 运行、`ctrl` 杀掉内核不崩**：未做——`u55sh` 是管道演示程序非交互 shell，依赖第 57 轮进程模型
- [ ] 把第 54 轮的 7 个 `cmd_*` 迁出 `kernel.c`（销技术债）：未做，第 57 轮随交互 shell 一起

## 第 56 轮：VFS + /dev + /proc（2026-11-16 完成抽象层 + 伪文件系统）

- [x] `vfs.c/h` + `vnode_ops`（lookup/read/listdir/stat）+ 8 项 mount 表 + 最长前缀；`test_vfs_host.c` 35/35
- [x] devfs（`/dev/null`/`zero`/`console`/`tty`）、procfs（`/proc/meminfo`/`uptime`/`version`/`cpuinfo`/`mounts`），开机静默挂载
- [x] `cat /proc/meminfo` 真实 `mm_stat` 数据，用户态与内核 shell 对账一致（8192/8188KB）
- [x] `SYS_vfsread`/`SYS_vfsinfo`（20/21）走 syscall；老路径零扰动（29 条 golden 逐字一致）
- [x] `test_round56.py` 本地 TCG 19/19；全套回归 15/15
- [ ] **跨挂载点 `cp`**：只做到跨挂载点读，`cp` 写文件到 procfs 未做（procfs 只读，语义上不该可写）
- [ ] VFS 全量替换上层（`fs.c` 降级 loongfs 后端、所有 `fs_*` 改 `vfs_*`、加 FAT32/EXT2 `/mnt` 挂载）：未做，留后续轮次
- [ ] `sysctl`/`free`/`uptime` 从命令补成 `/proc` 伪文件并删命令：`/proc` 侧已补，命令式保留未删（保基线）

## 第 57 轮：进程模型（最硬的活，顺序不可跳）

- [ ] ① 每进程 TTBR0 + 内核空间共用映射（保留 `-DSINGLE_AS` 回退开关）
- [ ] ② `fork`（先全拷贝，COW 后加）
- [ ] ③ `execve` + `exit` + `wait` + 僵尸回收
- [ ] ④ 全局 `g_fd` 改 `proc->files`（不改则 `du` 结果直接错）
- [ ] `vmalloc` 内核栈（销第 54 轮遗留）
- [ ] 进程隔离 fault 测试；`sh -c "ls"` 跑完进程表无残留

## 第 58 轮：TTY + 信号 + 作业控制

- [ ] `tty.c`、termios raw/cooked、回显、行编辑
- [ ] SIGINT/SIGTERM/SIGKILL/SIGCHLD + `kill`
- [ ] `Ctrl-C` 打断前台；`&` / `fg` / `bg`

## 第 59 轮：socket + UDP + DNS

- [ ] BSD socket 接口层（socket/bind/connect/send/recv/listen/accept）
- [ ] UDP、DNS 解析、用户态 `wget` 取回 HTTP 页面并存盘

## 第 60 轮：ZFS-lite 快照

- [ ] COW 写路径 + 延迟回收队列
- [ ] `snapshot create/list`、`rollback`

## 61+ 轮：差异化

- [ ] Jail-lite（`jail create` / `jls` / `jexec`）
- [ ] 可量化输入延迟 cycle 计数器（`<1ms` 硬数据）
- [ ] virtio-vsock / virtio-serial 文件服务（替代 b64）
- [ ] 启动时间 `<300ms` / 内核 `<1MB` CI 门槛
- [ ] SMP GIC CPU interface 收尾
- [ ] 自举工具链（长期方向）
