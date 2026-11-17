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

## 第 57 轮：进程模型（2026-11-16 完成地基 + 合作式 fork/wait，最硬的活第一步）

- [x] ① 每进程独立用户地址空间：复制用户 L3 表 + 独立物理页池，切 `l2_ram[1]` 基址 + TLBI；内核半共用（等价 TTBR0 用户半，KPTI 暂不做，符合文档口径）
- [x] ② `fork` 全拷贝：`SYS_fork` 逐页复制父用户区到子独立物理页（保留父 PTE 权限）；`test_proc_host.c` 30/30 + TCG 实测「子改 0x2222 父仍 0x1111」
- [x] ③ `wait`/`procexit`/僵尸回收：`SYS_wait` 让出/回收、`SYS_procexit` 退出码回传、二次 run 进程表无残留、`mmstat audit=0`
- [x] `proc.c/h` 进程表状态机（分配/僵尸/回收）+ 800B 帧整体拷贝上下文切换（boot.S 零改）
- [x] `getpid` 门控增强（单进程恒 1，老程序逐字不变）；syscall 22~25；golden 29 条逐字一致
- [x] `test_round57.py` 本地 TCG 21/21；全套回归 15/15；BSS 余量 16KB
- [x] ③ 真 `execve` —— **第 58.0-A 轮完成**（见下；第 57 轮时占位返回 -1）
- [x] ④ 全局 `g_fd` 改 `proc->files` —— **第 58.0-A 轮完成**（子进程走自己的 fd_table）
- [ ] 抢占式调度 / 时间片 / 多进程并发 —— 58.0-B（本轮合作式一层 fork）
- [ ] COW —— 58.0-C（用户明确放 VM/页引用计数之后，不赶版本）
- [ ] `vmalloc` 内核栈 / 用户 mmap —— 58.0-B/60.0
- [ ] 2.3 shell 迁出内核（需独立内核栈 + 交互 shell）—— 58.0-C/59.0

> 用户新排期（2026-11-17）：重心从「功能数量」转向「补内核六大核心能力」
> （进程/线程/内存/权限/文件系统/网络），按 58.0-A→58.0-B→58.0-C→59.0… 逐步推进。

## 第 58.0-A 轮：每进程 FD 表 + 完整 execve（2026-11-17 完成）

- [x] `fdtab.c/h`：每进程 FD 表 + 全局打开文件描述(OFD)池 + 引用计数；宿主 `test_fdtab_host.c` 47/47
- [x] fork 共享父 OFD（`ref++`，父子同 fd 号指向同一 ofd / 共享 off）；close/exit `ref--` 归零释放
- [x] 子进程 `pid>=2` 的 open/read/close/lseek/fstat/dup/dup2 走自己的 `fd_table`；pid=1 老 g_fd 逐字不变
- [x] 完整 execve：`load_elf_current()` 校验/PT_LOAD/bss/参数区/陷阱帧改写；失败回滚（阶段一全校验）、成功不返回、pid/fd/cwd 保留
- [x] `user/u58_fdproc.c`（FD 独立）、`user/u58_exec.c` + `u58_target.c`（execve）、`u58_badexec.c`（回滚），经 b64 注入 /home
- [x] `test_round58.py` 本地 TCG **22/22**（含 execve 坏路径失败回滚）；全套回归 15/15；golden 29 条逐字一致；stage5b 33/33
- [x] BSS 余量 16KB（OFD 池仅 192B）；零新增 `cmd_*`
- [ ] 真机 KVM 双路：本地 x86_64 无 KVM；NAS SSH 已恢复（2026-11-16）但本轮未在 NAS 上启动判据脚本
- [ ] 子进程的 pipe 层（fd 6..23）仍全局、execve 第二参是打包参数行非 argv[]、pid=1 仍走全局 g_fd —— 诚实边界（见逐项状态 §零点七）

## 第 58.0-B 轮：独立内核栈 + 抢占式调度（先单核）

- [ ] 每进程/线程独立内核栈 + 每线程独立上下文
- [ ] 进程状态机（RUNNABLE/RUNNING/SLEEPING/ZOMBIE）+ 时间片 + 优先级
- [ ] 定时器抢占 + 调度队列；两个用户进程真正交替执行
- [ ] 先单核稳定，SMP 留到 65.0

## 第 58.0-C 轮：COW + 用户态 Shell

- [ ] COW（在稳定 VM + 页引用计数之后；本轮 fdtab 的 ofd ref 已打样）
- [ ] Shell 自身作为 EL0 程序运行；迁出第 54 轮 7 个 `cmd_*`（销 §六技术债）
- [ ] `ls|grep|wc` 升级为三进程并发管道；`ps`/`kill`/`sleep`/`&`/`fg`/`bg`

## 第 59.0 轮：TTY + Signal + Shell 完善

- [ ] `/dev/tty`、termios raw/cooked、回显、行编辑、前台进程组
- [ ] SIGINT/SIGTERM/SIGKILL/SIGCHLD/SIGSEGV/SIGALRM

## 第 60.0 轮：VM + mmap + COW 收口

- [ ] `mmap`/`munmap`/`mprotect`/Page Fault/共享内存/vmalloc/Guard Page
- [ ] Swap 最后做（先物理页生命周期与虚拟映射正确）

## 第 61.0 轮：VFS 完整化 + FAT32

- [ ] VFS 全量接管（`fs_*` 改 `vfs_*`）、FD 统一接入 VFS、VFS 写接口
- [ ] FAT32（优先，可读常见 U 盘/分区）、EXT2、符号/硬链接

## 第 62.0 轮：Socket + UDP + DNS

- [ ] BSD socket（socket/bind/listen/accept/connect/send/recv/shutdown）、IPv4、UDP、DNS、DHCP

## 第 63.0 轮：TCP 完整化 + IPv6

- [ ] TCP 服务能力和 IPv6；目标 `ping`/`nslookup`/`wget`/`ssh`

## 第 64.0 轮：安全体系

- [ ] login/UID/GID/chmod/chown/ACL/setuid/Capabilities/MAC/ASLR/W^X
- [ ] **公开 Git 历史中泄露的 NAS 凭据真实轮换**（仅清理历史不够，必须改口令）
- [ ] 参考 FreeBSD Capsicum + OpenBSD 设计；KDF 改进、每用户独立 Salt

## 65.0+ 轮：SMP / 生态 / 差异化

- [ ] 65.0 SMP 多核真正投入调度；66.0 包管理器；67.0 Jail-lite；68.0 桌面/服务器完善
- [ ] 可量化输入延迟 cycle 计数器（`<1ms` 硬数据）
- [ ] virtio-vsock / virtio-serial 文件服务（替代 b64）；启动 `<300ms` / 内核 `<1MB` CI 门槛
- [ ] 自举工具链（长期方向）
