# LoongOS 开发者指南（59.0-A）

> 面向第三方开发者：如何为灵珑系统 / LoongOS 编写用户态程序、调用系统 API、参与内核开发。
> 本文对应内核版本 `LoongOS 59.0-A-devapi`。所有示例均已在 QEMU TCG 端到端验证。

---

## 1. 这是什么

LoongOS 是一套从 `boot.S` 开始手写的 64 位 ARM（aarch64）裸机内核，不基于 Linux 或 BSD。开发者视角下它提供：

- 一个 **SVC 系统调用 ABI**（用户态 ELF ↔ 内核，共 36 个号，0~35）
- 一套 **无 libc 的用户态开发方式**：`-nostdlib` 静态 ELF，链表加载地址
- 一个纯 C 头文件 SDK **`loslib.h`**，把所有 syscall 封装成 `los_*` 函数
- 一个可运行的 **API 演示程序** `user/u60_api.c`

用户态程序跑在 **EL0**，内核在 **EL1**；用户程序只能经 `svc #0` 与内核通信，触碰内核地址会被拒绝或杀进程（见 `user/bad.c`）。

---

## 2. 开发环境

| 项 | 要求 |
| --- | --- |
| 交叉编译器 | `aarch64-linux-gnu-gcc`（本项目用 13.3） |
| 模拟器 | `qemu-system-aarch64`（`-machine virt,gic-version=2 -cpu cortex-a57`） |
| 宿主构建 | Linux，Python 3（判据脚本） |
| 可选 | Docker（LOONG 软件源端到端判据） |

无需 libc、无需第三方库——SDK 全部是内联函数，只发 SVC。

---

## 3. 编译与运行你的第一个程序

### 3.1 最小程序

```c
/* myapp.c */
#include "loslib.h"

int main(void) {
    struct utsname u;
    los_uname(&u);
    los_puts("Hello from ");
    los_puts(u.sysname);
    los_puts("\n");
    los_exit(0);
    return 0;
}
```

### 3.2 编译命令

```sh
aarch64-linux-gnu-gcc \
  -nostdlib -ffreestanding -static -fno-stack-protector -fno-pie -O2 -Wall \
  -Ttext=0x40240000 -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
  -I本体/user -o myapp.elf myapp.c
```

> `-Ttext=0x40240000` 必须落在 `[USER_BASE, USER_END)` 内（当前 `USER_BASE=0x4023c000`，`USER_END=0x40400000`）。写错链接地址会被内核 ELF 加载器拒绝。头文件路径指向 `本体/user`，`loslib.h` 会自动包含 `user.h`、`usys.h`、`proc.h`、`sched.h`。

### 3.3 放进系统并运行

用系统的 base64 文件通道注入（任意二进制安全）：

```sh
# 在 LoongOS shell 里：
b64in /home/myapp
# 逐行粘贴 base64（76 字符/行），输入 . 结束
run /home/myapp
```

或参与内核构建：把源文件加进 `build.sh`（参照 `u60_api.c`），编进镜像后直接 `run`。

---

## 4. 调用约定（ABI）

- `x8` = syscall 号，`x0/x1/x2` = 参数，返回值在 `x0`
- 返回约定：**0 = 成功，-1 = 失败**（`getrandom` 返回实得字节数）
- 用户传入的指针必须落在用户区 `[USER_BASE, USER_END)`，内核逐一用 `u_range_ok()` 校验；非法指针返回 -1
- 结构体全部**定长、无指针、显式对齐**，用户态与内核用同一头文件（`usys.h`）保证逐字节一致

不要直接内联汇编发 SVC——用 SDK 的 `los_*` 封装即可；确实需要时用 `user.h` 的 `sys_call`/`sys_call3`。

---

## 5. API 参考

### 5.1 基础 I/O 与进程（历史 ABI，0~27，逐字稳定）

| SDK 函数 | syscall | 说明 |
| --- | --- | --- |
| `los_puts(s)` / `los_putc(c)` / `los_putdec(v)` / `los_puthex(v)` | 0 | 串口输出（老 `write(ptr,len)` 用法） |
| `los_open(path)` / `los_read(fd,b,n)` / `los_close(fd)` | 2/3/4 | POSIX 风格文件（fd 1/2 是串口） |
| `los_getcwd(b,n)` / `los_chdir(p)` | 6/7 | 当前目录 |
| `los_getpid()` | 8 | 当前 pid |
| `los_stat/fstat/dup/dup2/pipe` | 9~13 | 元数据 / 复制 fd / 管道 |
| `los_mkdir/unlink/rename/creat/getdents` | 14~19 | 目录与文件操作 |
| `los_vfsread/vfsinfo` | 20/21 | 读 `/proc` `/dev` 伪文件 |
| `los_fork/wait/procexit/execve` | 22~25 | 进程模型（合作式） |
| `los_yield()` / `schedstat` | 26/27 | 让出 CPU / 调度统计 |

### 5.2 开发者 API（59.0-A 新增，28~35）

这些是**纯转发型** syscall：复用内核已有能力，无新增算法。

#### `long los_clock_gettime(int clk, struct utime *t)`
- `clk=0` 墙钟：PL031 RTC 的真实 Unix 秒（`t->nsec=0`）
- `clk=1` 单调钟：`cntpct_el0 / cntfrq_el0`
- `struct utime { u64 sec; u64 nsec; }`（16B）
- 返回 0 / -1

#### `long los_getrandom(void *buf, unsigned long len)`
- virtio-rng 真随机数
- 返回**实得字节数**（可能小于 len）；无 virtio-rng 设备返回 -1
- 应用需检查返回值，不能假定填满

#### `long los_uname(struct utsname *u)`
- 填充系统信息（4×16B 定长字符串）：
  - `sysname="LoongOS"` `release="59.0-A"` `version="loong-dev-api"` `machine="aarch64"`
- 返回 0 / -1

#### `long los_getppid(void)`
- 返回父进程 pid；根进程（pid=1）的父返回 0

#### `long los_nanosleep(const struct utime *t)`
- 教学实现为**忙等**（非真正阻塞调度），上限 2 秒防卡死 shell
- 返回 0 / -1

#### `long los_fsstat(struct ufsstat *s)`
- 内存盘块位图统计；`struct ufsstat { u32 used_bytes, total_bytes, blocks, reserved; }`

#### `long los_meminfo(struct umeminfo *m)`
- kmalloc 堆统计；`struct umeminfo { u32 used_bytes, peak_bytes, live_objs, fail_cnt; }`

#### `long los_uptime(struct utime *t)`
- 单调运行时间（同 clock_gettime 的单调钟）

---

## 6. SDK（`loslib.h`）设计

`user/loslib.h` 是开发者的单一入口，层级：

```
u60_api.c
  └── loslib.h        ← 你 include 这个
        ├── user.h     ← sys_call/sys_call3 + 老 u_* 封装
        │     ├── usys.h   ← syscall 号 + struct ustat/utime/utsname/...（ABI 契约）
        │     ├── proc.h   ← fork/execve/wait/procexit 号
        │     └── sched.h  ← yield/schedstat 号
        └── los_* 友好封装
```

提供：
- 输出工具：`los_puts/putc/putdec/puthex/print_time`
- 全部 syscall 的 `los_*` 封装
- 统一返回约定与注释

新增 API 的标准流程：①`usys.h` 加号与结构 → ②`kernel.c` 的 `svc_dispatch` **末尾**追加分支 → ③`loslib.h` 加封装 → ④写 `user/uXX_*.c` 示例 → ⑤`build.sh` 登记 → ⑥宿主单测 + `test_roundXX.py` 判据 → ⑦golden 29 零扰动。**绝不修改 0~27 的分支**。

---

## 7. 完整示例

见 `user/u60_api.c`（可运行范例），演示了 uname、墙钟/单调钟、uptime、随机数、fsstat、meminfo、pid/ppid、nanosleep 的全部调用与打印。判据 `test_round60.py` 会逐项核对其输出数值。

---

## 8. 约定与边界（诚实声明）

1. **无 libc**：没有 malloc/printf/标准库，字符串与输出用 SDK 自带工具。动态内存需自行经 syscall 暴露（当前未开放用户态 brk）。
2. **教学 TCP 单连接**：网络栈是 RFC 子集，无乱序重组/拥塞控制，一次只维护一条连接；LOONG 下载已针对常见场景修复并端到端验证，但未覆盖所有网络拓扑。
3. **明文 HTTP**：LOONG 软件源走明文 HTTP，仅限可信内网，勿暴露公网。
4. **nanosleep 是忙等**：不触发调度让出；需要让出用 `los_yield()`。
5. **真机 KVM 未执行**：判据环境为 QEMU TCG；宿主无 aarch64 KVM，未做真机 KVM 双路验证，不冒充。
6. **golden 零扰动**：所有新 API 追加在 ABI 末尾，29 条 golden 行为基线与老 ELF 程序逐字不变。

---

## 9. 判据与质量保障

| 测试 | 覆盖 | 结果 |
| --- | --- | --- |
| `test_tcpbuf_host.c` | TCP 接收缓冲读写游标一致性（修复前 FAIL，修复后 PASS） | 6/6 |
| `test_round60.py` | 开发者 API 端到端 + golden 零扰动 | 16/16 |
| `test_loong.py` | LOONG 包管理器端到端（含大包下载） | 16/16 |
| 全套宿主单测 | loong 89 / sched 72 / net 52 / fs 30 / proc 30 / fdtab 47 / vfs 35 / usysfd 57 / mmrtc 37 / crypto 16 | 全过 |
| `run_regression_r.sh` | 15 项资源/长稳/网络/安装回归 | 见判据报告 |

> 原则：不接受"串口自报通过"。落盘字节数用 `ls` 独立读、SHA256 独立重算、程序输出必须是真实 ELF 在 EL0 跑出来的。

---

## 10. macOS 风格桌面（60.0-M：内核原生）

LoongOS 在 `desktop.c`（类 Windows 桌面）之外，新增第二套**内核原生**的 macOS 风格桌面，逻辑与 UI 分离，交互规则严格对齐 macOS 原生行为。

### 10.1 进入

在串口 shell 敲 `macdesktop`（`q` 或 Ctrl+Shift+F1 退出）。它是一条**新增可选命令**，不进 help、不改 golden。

### 10.2 模块划分

| 文件 | 职责 |
| --- | --- |
| `macwm.h` / `macwm.c` | **交互内核**：纯逻辑、无硬件依赖、宿主可单测。`struct mac_state`（430B）承载 App/窗口/焦点/Dock 全部状态；`mac_dispatch(evt,a,b)` 是唯一状态变更入口 |
| `macdesktop.c` | **渲染 + 输入适配**：把 `mac_state` 画成三段菜单栏/Dock/窗口（复用 `gfx.c` 原语），把键鼠翻译成 `mac_dispatch` 事件 |
| `test_macwm_host.c` | 宿主逻辑单测（57 条） |
| `test_macdesktop.py` | QEMU 端到端（9 项，QMP 读 `g_ms.active_app` 验证焦点联动 + screendump） |

### 10.3 核心交互语义（与 Web 版 wm_core.js 逐条一致）

- **App / 窗口完全解耦**：一 App 拥有 0..N 窗口；红按钮只销毁该窗口，App 仍后台运行、Dock 圆点保留；只有 `quit` 才结束进程。
- **全局焦点是 `active_app`**（不是活动窗口）：桌面永远属于 Finder，不存在"无焦点"态；三段菜单栏中间段只绑定 `active_app`。
- **焦点联动三路径**：点窗口→激活其 App；点桌面空白→激活 Finder；Cmd+Tab（本内核以 Ctrl+Tab 代）→按 MRU 切换——三者都同步刷新菜单栏。
- **红黄绿按钮**：红=关窗不退出；黄=最小化进 Dock 右侧（点击缩略图恢复）；绿=全屏独占一个 Space；zoom 为普通放大不建 Space。
- **Dock**：图标=App 实例，圆点=运行中；左侧 Finder/启动台常驻不可移除；点未运行→启动，点已运行→不新建进程只切前台；常驻项拖出无效。

### 10.4 扩展一个 App

在 `macwm.h` 的 `enum mac_app` **末尾**追加编号、`macwm.c` 的 `mac_defs[]` 与菜单表加对应项，即可出现在 Dock 与开始菜单；渲染主题色在 `macdesktop.c:app_tint()` 加分支。逻辑事件仍走 `mac_dispatch`，无需改输入层。

### 10.5 内存与地址

新增模块使内核 `.text` 增长约 12KB，经链接脚本 4KB 对齐链传导顶到用户区；按项目历史惯例把 `USER_BASE` 由 `0x4023c000` 上移到 `0x40244000`、用户程序 `-Ttext` 同步到 `0x40248000`（落在新用户区内）。`USER_END=0x40400000` 不变，128MB RAM 空间充裕。`run_regression_r.sh` 15/15、`test_round60.py` 16/16 验证用户程序与 golden 零扰动。
