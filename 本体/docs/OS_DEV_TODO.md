# OS_DEV — 灵珑系统 | LoongOS（自研操作系统，aarch64 裸机 / QEMU 本地 / 零成本）

**2026-10-24 全面更名**：MyOS → **灵珑系统 | LoongOS**（目录 myos/ → loongos/、
FS_MAGIC `"MYOS"`→`"LOOS"`、头文件保护 `MYOS_FS_H`→`LOONGOS_FS_H`、开机横幅与演示文本同步换）。

路线图（用户 2026-10-24 拍板：本地 QEMU 免费路线，不租服务器，周末工程随时可停续）：

- [x] **阶段 0：内核点亮**（2026-10-24 上午）——PL011 串口 + 回显 shell
- [x] **阶段 1：异常向量 + 键盘中断**（2026-10-24 下午）——16 槽向量表、GICv2、RX 中断、
      崩溃现场转储 + ELR+4 自愈（UDF/BRK/数据中止三连验证过）
- [x] **阶段 2：内存管理（MMU 分页）**（2026-10-24 中午）——4KB 页表恒等映射、text 只读/
      data 可写/设备 Device 三类权限、开 MMU 标准序列、EL0 用户态演示（读内核被逮自愈 + SVC 回内核）、
      RWX LOAD 警告清零（PHDRS 三段）
- [x] **阶段 3：多任务调度**（2026-10-24 中午）——Generic Timer 10Hz 抢占、异常帧换栈式
      上下文切换（handle_irq 返回帧指针 + irq_entry `mov sp,x0`）、造帧创建任务、
      两任务轮转、`t` 命令任务表；阶段 1/2 能力零回归
- [x] **阶段 4：文件系统 + 行模式 shell**（2026-10-24 下午）——1MB 内存盘（superblock+位图+
      inode 表+目录项，10 直接块+1 间接块，最大 2MB+40KB 文件）、Unix 单根 `/`、
      `ls/cat/write/mkdir/rm/df/help` + 老功能收编（`mmu/tasks/el0/crash udf|brk|abort|wronly`）、
      行输入+退格；第二个编译单元 fs.c；内核 28/0、宿主机压测 8/8；阶段 1~3 能力零回归
- [x] **阶段 5 ①：root 与权限基建**（2026-10-24 傍晚）——用户 root(0)/demo(1000)、
      `/etc/passwd`（可读）+ `/etc/shadow`（salt+SHA-256 迭代 2000 轮，只存哈希）+ `/home/demo`（属 demo）；
      inode 加 uid/mode（FM_PRIVATE 私密位）；DAC=改/删属主或 root、建=父目录属主或 root、读默认公开
      （FM_PRIVATE 只许属主/root）；新命令 `whoami` / `su [用户]`（root 免口令直切，非 root 要口令、
      不回显；su 特权读 shadow 同 Unix setuid）；新编译单元 crypto.c（SHA-256 KAT 9/9）；
      错误码 FS_DENIED 分级提示；crypto.c 尾块 off-by-one 被 FIPS 向量当场抓住；
      阶段5 端到端 35/35、阶段4 回归 28/0、crypto KAT 9/9
- [ ] **阶段 5 ②：ELF 加载器**——从 fs 读 ELF 进 EL0 跑用户程序（配合权限）
- [ ] **阶段 5 ③：cwd + 相对路径**
- [ ] **阶段 5 ④：重定向或管道（二选一）**
- [ ] 更多命令（touch/mv/stat）视时间加
- [ ] **阶段 6（远期）：持久化/用户态**——virtio-blk 驱动（fs.c 只需换 bread/bwrite 两层）、
      用户程序从 fs 加载进 EL0 跑

布局：`myos/{boot.S, kernel.c, fs.h, fs.c, linker.ld, build.sh, run.sh, test_stage4_fs.py}`
留痕：`myos/docs/检查与修复记录.md`（每轮 bug 清单 + 可重跑验证命令）

## 阶段 5 开工前先读（阶段 4 踩的坑）
- **CPACR_EL1.FPEN 必须在跑任何 C 代码前置 11**（boot.S 已做）：GCC 会把大块内存清零
  自动向量化成 NEON（`movi v0.4s`），没开就 EC=0x07 未定义异常，且**崩点离案发现场很远**
- **4KB 块缓冲绝不放栈上**（16KB 栈三层嵌套就爆）：fs.c 用单块静态 scratch；
  ⚠️ scratch 无锁——多任务要碰 fs 必须先加信号量（当前只有 shell 用，安全）
- **-O2 strict-aliasing 会吃掉类型双关读**：`*(struct inode *)(u8数组+off)` 在宿主机 -O0 正常、
  内核 -O2 读回垃圾；一律用 fs_memcpy 拷出 + 全局 `-fno-strict-aliasing`（已加进 build.sh）
- **`u32 arr[N] = {0}` 在 freestanding 下生成 memset 调用 → 链接炸**；大数组要清零走 fs_memset/fs_zero
- **bwrite 永远写满 4KB**：短结构体（super/dirent）必须先垫进整块缓冲，不能 `bwrite(b, &短结构体)`
- **交互测试断言必须剔除命令回显**（按命令前的缓冲区水位切片），否则断言匹配到自己打的命令 → 假绿
- **测试期望值别凭印象写**：ESR 打印全宽 hex、mmu 输出是 SCTLR 不是"已开启"——写完测试逐条手工复核

## 阶段 4 开工前先读（阶段 3 踩的坑）
- **PPI（16~31）中断也要 `GICC_EOIR`**——EOIR 条件写 `id>=16`；漏了它中断只响应一次
  （串口是 SPI33 不受影响，上 PPI 才暴露）
- **造帧创建任务**：栈顶-288 造初始帧（p[31]=ELR、p[32]=SPSR=0x345），RESTORE 即入函数；
  ⚠️ `tasks[i].stack_top` 忘赋值 = 拿 0-288 当帧地址，开机即崩
- **周期性判断别用共享 tick**：严格轮转下另一任务只看到奇数 tick，`tick%10==0` 永远假；
  数自己的运行片数（slices）
- **切换开关就三行**：`handle_irq` 返回帧指针 + `task_switch` 换 `tasks[].frame` +
  boot.S `irq_entry` 里 `mov sp, x0`；sync（崩溃转储）路径不用动

## 阶段 3 开工前先读（阶段 2 踩的坑）
- **T0SZ 决定起查级**：T0SZ=16→48bit 从 L0 查（要 L0 表）；我们用 T0SZ=25（39bit）从 L1 查
- **PTE=地址|属性 拼接前地址必须页对齐**——低 12 位脏值会污染 AP 权限位（__data_start 不对齐踩过，
  症状=MMU 开后全线写故障、转储寄存器怪异）
- **块描述符（L1/L2 2MB/1GB）绝不带 PTE_TABLE(bit1)**，带了会被当"下一级表指针"；
  L3 页描述符要 VALID|TABLE|AF|SH 全位
- **eret 前必须 `msr spsr_el1`**——忘了写就带着旧 SPSR（EL1h）跑，进不去 EL0
- 判"异常来自 EL0"用 EC 0x20/0x24（分档 EC），不是 ESR bit16（那是 ISV）
- `fill_l3` 类区间函数起止各自向下/向上取整到页
- 排障链：`-d int` 读 **IFSC/DFSC 低 6 位**：0x05/0x06/0x07=level1/2/3 translation fault、
  0x0D/0x0E/0x0F=level1/2/3 permission fault——一级一级往后推进就是好现象

## 阶段 1 沿用的坑
- 向量表槽位逐个 `.balign 128`；aarch64 `.align` 是字节语义
- QEMU pl011：RX=bit4 / TX=bit5；不开 TX 位；**别写 ICR**（会抹掉早期字符的 RX 状态 → 接收死锁）
- `GICD_ISENABLER` 写 `1<<(id%32)`；`msr daifclr, #2`（无裸 daifclr）
- 构建前 `rm -f *.o kernel.elf`；构建输出别用管道过滤吞错误
- 自动化测试用逐段延迟输入模拟真人打字；管道一次性灌入会撞 QEMU stdio-mux 排队怪癖
- 取证利器：`qemu -d int`（每次异常的 ESR/ELR/向量目标）
