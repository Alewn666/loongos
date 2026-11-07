# LOONGOS_KBD_TODO — 给 LoongOS 加 xHCI + USB HID 键盘驱动（阶段6①）

目标：飞牛虚拟机 ramfb 屏幕上能直接用 USB 键盘打字（不再只有串口能输入）。

- [x] 1. MMU 补映射：PCI ECAM(0x3f000000,16MB) + PCIe MMIO(0x10000000-0x3effffff) 设备属性
- [x] 2. PCI ECAM 枚举：扫 bus0-15 找 xHCI（class 0x0C0330），读 BAR0 + 开 Memory/BusMaster
- [x] 3. xHCI 控制器初始化：CAPLENGTH/HCSPARAMS → 复位 → DCBAA → 命令环(32TRB+Link) → 事件环(ERST) → 启动
- [x] 4. 设备上线：PORTSC 检测+端口复位 → Enable Slot → Address Device(BSR=0)
- [x] 5. 控制传输引擎：Setup/Data/Status 三段 TRB + 事件轮询；GET_DESCRIPTOR 设备/配置
- [x] 6. HID boot 键盘：找 class 03/sub01/prot01 + 中断 IN 端点 → SET_CONFIGURATION → SET_PROTOCOL(boot) → Configure Endpoint
- [x] 7. 键码翻译：boot 报告 8 字节 → ASCII（US 布局，shift 处理）→ ring_push（补 IRQ 临界区防双生产者竞争）
- [x] 8. 接线：kernel_main 初始化 + shell_loop/blocking_getline 轮询 usb_kbd_poll()
- [x] 9. 测试：TCG 冒烟（-device qemu-xhci -device usb-kbd + QMP sendkey 敲 help/whoami 验证回显与命令执行）—— **`python3 test_kbd.py` ALL PASS**
- [x] 10. KVM 复验 + 重打 ISO + 归档 NAS + 留痕 docs/检查与修复记录.md
  - [x] **KVM 崩溃已修（第十四轮）**：根因 = 端口扫描被 GCC 编成**后索引** `ldr w3,[x1],#16`；
        带写回的加载在 stage-2 中止里 **ISV=0** → KVM 走 NISV → QEMU 解不出这笔 MMIO → 给 guest 注入 data abort
        （`ESR=0x96000010`，看着像"对齐错误"，但 0x440 对齐到 64 字节，硬件不可能真报对齐错）。
        修法：`rd32/wr32` 改**内联汇编单次 ldr/str**，所有 MMIO 访问点统一走它们（禁止裸 `*(volatile u32*)`）。
        取证：宿主机 `kvm_guest_fault hsr=0x92000005`（ISV=0）↔ 正常 UART `hsr=0x93810006`（ISV=1）；
        KVM 下 `usb_xhci_port_read` trace 0 次、TCG 12 次；monitor `xp`/页表 dump 排除「QEMU 无区域」「DMA 冲页表」。
  - [x] 重打 ISO（22:49 修复后版，md5 `f0c77aec85ded1bf0c52ebf49e6e70b5`）
  - [x] 验证：TCG `python3 test_kbd.py` **ALL PASS**；KVM `sudo KVM=1 python3 test_kbd.py --kvm` **ALL PASS**；
        `python3 test_iso_boot.py` **12/12**
  - [x] 归档 NAS（22:52，md5 与工作区一致，临时文件 + mv 原子替换，属主还原 libvirt-qemu）
  - [x] 留痕 docs/检查与修复记录.md 第十四轮（三个假设 + tracepoint 证据 + 可重跑命令表）
  - [x] **追加修：真机多 USB 设备时抓错设备**（同一个 bug 的另一半）—— 用户 VM 有 3 个 USB 设备
        （tablet/mouse/keyboard），驱动只认「第一个有设备的端口」→ 抓到 tablet → 报「没找到 HID boot 键盘接口」
        → `return -1`，键盘（port7）压根没被试。修法 = 端口遍历循环，非键盘 `continue` 试下一个。
        **回归护栏**：`test_kbd.py` 的 QEMU 拓扑改成 tablet(port1)+mouse(port2)+kbd(port3) 复刻真机。
        验证：TCG/KVM 双路 ALL PASS（日志可见 5→跳过、6→跳过、7→命中）+ ISO 冒烟 12/12
        + **真机 `virsh send-key` 敲 `help` 上屏**（截图确认命令清单）

设计要点：
- 轮询事件环（10Hz tick 唤醒 wfi），不上 MSI-X/GIC 中断
- DMA 结构全用静态 64B 对齐数组（无分配器）；QEMU TCG/KVM 的设备 DMA 与 guest CPU 同一 host 一致性域，无需 cache 维护
- ring_push 加 daif 临界区：UART IRQ 与 kbd 轮询两个生产者可能互踩
  - [x] **追加：退格键/Delete（第十五轮）** —— 用户反馈「删除键没做」。根因不在键码表（`0x2A→'\b'` 本就有）
        也不在 shell（`if (c=='\b'||c==127)` 本就有），而是**帧缓冲控制台不吃 `\b`**：`fb_draw_cp()`
        只处理 `\n/\r/\t`，`\b` 掉进画字符分支被画成 `?` → 屏幕上删不掉（串口终端本来就能吃，所以只接串口测不出来）。
        修法：`fb_draw_cp` 加 `\b` 分支（光标左移一格 + 抹黑该格）；`hid_map` 补 `0x4C Delete → 127`
        让标着 Delete 的键也能删。测试加用例：`helz` → 退格 → `p` → 回车应执行 help。
        验证：TCG/KVM 双路 ALL PASS、ISO 冒烟 12/12、真机截图确认 `z` 被擦掉并成功执行 help。
