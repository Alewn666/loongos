LoongOS / 灵珑系统 —— 第五十九轮 59.0-A 交付报告
时间：2026-11-20 (GMT+8)
主题：修复真机下载大包 bug + 开放开发者 API/SDK + 开发文档 + 开发 PPT

一、用户需求（本轮四条）
  1. 「计算机用着用着还是有问题，赶紧修，做严谨些，预防 bug」——指上一轮真机上
     `loong install hello` 卡在 `poff=160` 下载失败。
  2. 为开发者多开放 API / 让开发更好做。
  3. 做一份开发 PPT。
  4. 做一份开发文档。

二、核心 bug 根治（真机大包收不全）
  现象：真机桥接网络下载 7.7KB .lpkg 停在 `poff=160 / dlen!=5624`；QEMU+SLIRP 16/16 通过。
  根因（子代理走查 + 本人逐行核对确认）：tcp.c 接收缓冲读写游标不一致——
    tcp_rx 写入用 t_rx+t_rx_len，tcp_recv 读取用 t_rx+t_rx_head。
    应用读走一部分使 head>0 后，新段写到 t_rx+len 会落进未读区 [head, head+len) 内部，
    覆盖尚未读走的字节。SLIRP 小段每次读空(head=0)掩盖了它，真机满 MSS 段令 head 持续>0 必现。
  复现：新增 test_tcpbuf_host.c（直接 include tcp.c，桩掉硬件依赖，构造 head>0 后新段到达）。
    修复前 4/6 PASS，关键两项 FAIL（段A未读的 2000B 中 1000B 被覆盖，段B 也有 1000B 错位）。
  修复：读写两侧都做游标归一化——把 [head,head+len) 搬到 [0,len)、head 归 0，
    写入点 t_rx+len 永远紧邻未读区尾。修复后 6/6 PASS。
  顺带加固（可确认的真实隐患）：
    - loong_db_remove 判空顺序前移（原 !db 写在 loong_db_find 之后）
    - HTTP body 超 64KB 不再静默截断（kfree + total=0 触发重试）
    - loong_verify_sha 入口校验 hex 满 64 字符，防越界读
    - send_seg 通告窗口加下溢钳位

三、开发者 API（59.0-A，syscall 28~35，纯转发已有内核能力）
    28 clock_gettime  墙钟(PL031)/单调(cntpct)   29 getrandom    virtio-rng 真随机
    30 uname          LoongOS/59.0-A/aarch64      31 getppid      父进程 pid
    32 nanosleep      教学忙等(上限2s)           33 fsstat       内存盘块位图统计
    34 meminfo        kmalloc 堆统计             35 uptime       单调运行时间
  设计原则：全部追加在 svc_dispatch 末尾，不碰 0~27，golden 29 条逐字零扰动。
  配套：
    - user/loslib.h：开发者单一入口 SDK，los_* 封装 + 输出工具，无 libc，只发 SVC
    - user/u60_api.c：可运行范例，演示全部 8 个 API
    - usys.h 新增共享结构 utime/utsname/ufsstat/umeminfo；proc.h SYS_max 改条件定义消除重定义警告
    - build.sh 登记 u60api.elf；LOS_VERSION → 59.0-A-devapi

四、开发者文档（docs/LoongOS开发者指南.md，8KB）
  覆盖：架构与链路、开发环境、编译/注入/运行、调用约定、36 个 API 全参考、
  loslib.h 层次、完整示例、约定与诚实边界、判据体系。

五、开发演示 PPT（/workspace/LoongOS开发者指南_59.0-A.pptx，13 页）
  沿用宣传演示的「科技深空」视觉规范（深色底 #0B1521 / accent #63A7D4 / Noto CJK）。
  13 页：封面 / 目录 / 体系总览 / ABI / syscall 全景 / 新API(1,2) / SDK / 编译运行 /
  TCP修复 / 判据体系 / 边界路线 / 结尾。文本框越界检查：无（封面光晕椭圆除外，属设计）。

六、验证（全部真实判据，不靠串口自报）
  - test_tcpbuf_host.c：6/6（修复前关键项 FAIL）
  - test_round60.py（开发者 API 端到端 + golden 零扰动）：16/16
  - test_loong.py（LOONG 端到端，含大包下载）：16/16
  - 全套宿主单测：loong 89 / sched 72 / net 52 / fs / proc 30 / fdtab 47 /
    vfs 35 / usysfd 57 / mmrtc 37 / crypto 16 —— 全过
  - run_regression_r.sh 15 项：PASS=15 FAIL=0，RC=0（golden 基线逐字一致）
  - 内核直接启动冒烟：进 stage5 shell 正常
  - ISO 内 kernel.elf 与本地字节完全一致（cmp 通过）

七、交付物
  - 内核 kernel.elf md5 = e8622b5e67a7aad9c7618cfe6e46d8ae
  - ISO  iso/自研内核ISO/loongos.iso md5 = 8e709e09ea33ac1dd463af2738079c23（19,130,368 B）
  - GitHub：commit bf7d37d 已推送 origin/main，本地=远端一致
    （核心 be61313 + 报告 8d1764f + 清单 bf7d37d 三笔全部推送成功）
  - 提交前扫描：无明文凭据/口令泄露（password=PASS 为变量引用，非明文）

八、诚实边界 / 未完成项
  ⚠️ NAS 同步本轮【失败】，原因不在沙箱侧：
    - NAS 的 /vol1 已进入 emergency read-only（ext4 "Remounting filesystem read-only"）。
    - dmesg 显示底层 md/RAID I/O 错误（md: super_written error=-5、Buffer I/O error on dev md0、
      EXT4-fs Detected aborted journal），属 NAS 存储/磁盘级故障。
    - 表现：SFTP 读取正常（能算 md5），但所有写入报 OSError: Failure；touch 报 Input/output error。
    - 后续连 SSH exec 都 Timeout opening channel，疑 NAS 正在重启或 sshd 因盘故障卡死。
    - 处理原则：未在坏盘上执行 fsck 或强制 remount rw（可能造成数据损坏），需用户本人在 NAS 侧
      检查 RAID/磁盘健康、恢复读写后重跑 `python3 /root/.loongos/auto_sync.py` 即可补同步。
    - 影响：GitHub 是主交付渠道（已完整同步）；NAS 仅是镜像，本轮镜像未更新，源代码/ISO/文档
      均在 GitHub 与本地工作区完好。
  - 真机 KVM 仍未执行（宿主无 aarch64 KVM），判据为 QEMU TCG，未冒充双路。
  - LOONG 源服务仍在 NAS 1100 端口运行（上一轮部署），但同样受 NAS 只读影响，重启后需复验。
