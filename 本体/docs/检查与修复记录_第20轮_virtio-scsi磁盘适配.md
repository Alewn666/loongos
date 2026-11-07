# 第 20 轮：virtio-scsi 磁盘适配（飞牛/libvirt 的 SCSI 虚拟机终于能装系统）

- 日期：2026-10-25
- 目标：让纯自研 LoongOS 能识别并安装到用户虚拟机的 **SCSI 硬盘**（飞牛把硬盘挂在 `bus='scsi'`，即 QEMU 的 virtio-scsi 半虚拟化设备 `1af4:1004/1048`）

## 结论

**已打通**：`test_scsi_install.py` 三阶段全过 —— 从 ISO 安装 → 只挂硬盘启动 → 重启后文件仍在。

```
① 从 ISO 启动并 install yes → [install] START / MBR-OK / ESP-OK / FMT-OK / DONE
② 只挂硬盘启动 → [scsi] 使用 target=0 lun=1 / [disk] virtio-scsi 就绪，容量 96 MB
                 [fs  ] MOUNTED-DISK lba=4096 blocks=24064
③ 再启动一次 → cat /home/scsi_ok.txt → scsi-persist-ok
ALL PASS —— SCSI 盘：装盘 → 从盘启动 → 文件持久 全通过
```

## 这一轮踩到的 5 个真问题（每个都实机复现过）

1. **LUN 编码错**：QEMU 的 `virtio_scsi_device_get()`（hw/scsi/virtio-scsi.c）要求
   `lun[0] == 1`、`lun[2] == 0 或 0x40..0x7f`、`LUN = (lun[2]<<8)|lun[3]`。
   我们原来发 `lun[0]=0` → 所有请求被判 BAD_TARGET（response=3），现象是"所有 LUN 都读不到容量"。
   参考 Linux `drivers/scsi/virtio_scsi.c:516-519`：`lun[0]=1; lun[1]=id; lun[2]=(lun>>8)|0x40; lun[3]=lun&0xff`。

2. **响应描述符位置错**：VIRTIO 1.0 且未协商 `VIRTIO_F_ANY_LAYOUT` 时，QEMU 把**第一个可写描述符**当响应头
   （virtio-scsi.c 里 "Old BIOSes left some padding" 那段）。原来把响应放最后 → 响应被写进数据缓冲区，
   永远读到 `response=0xff`。

3. **写命令描述符顺序错**：VIRTIO 要求"可读描述符全部在前、可写在后的链式顺序"。
   写盘时 `[请求][响应][数据]` 会被 QEMU 判 `Incorrect order for descriptors` 并让设备进入错误状态。
   正确（同 Linux）：读 = `[请求][响应][数据]`，写 = `[请求][数据][响应]`。

4. **响应描述符漏 NEXT 位**：读命令时响应后面还挂着数据段，响应描述符必须带 `NEXT`，否则环链在响应处截断
   → QEMU 认为"没有数据缓冲区"→ 回 `OVERRUN(response=1)`。排查时用 `TUR(0x00)`（无数据命令）能正常返回
   而 `READ CAPACITY` 失败，正是这个特征。

5. **`used.idx` 快照取太晚（竞态）**：`before = vs_used.idx` 必须取在**通知设备之前**。
   否则 QEMU 处理极快时会先完成，我们再去等"下一次"完成 → 随机超时（`r=4`），
   表现是装系统时偶发 `[scsi] WRITE fail ... r=4` 导致 `[install] FAIL format/esp`。

## 附带发现（不是 SCSI 的事，但卡了测试两小时）

- **UEFI 的 NVRAM（VARS）会记住旧启动项**：反复用同一个 `AAVMF_VARS.fd` 测试时，里面残留指向 ISO/PXE 的
  BootOrder，装好的盘"起不来"其实固件根本没去扫盘。测试脚本必须**每次用全新 VARS**。
- **EDK2 的 virtio-scsi 按 LUN 顺序枚举**：只挂 LUN1（硬盘）时不会去看它，必须保留 LUN0
  （哪怕空光驱托盘）。用户的虚拟机布局（光驱 LUN0 + 硬盘 LUN1）正好满足，拔掉安装盘即可从硬盘启动。

## 验证入口（可重跑）

```bash
cd <工作区>/loongos
sh build.sh && sh build_iso.sh
sudo LOONGOS_GUARD_OFF=1 python3 test_scsi_install.py     # 三阶段，约 4 分钟
```

`test_scsi_install.py` 复刻用户虚拟机布局：`virtio-scsi` 控制器 + 光盘 LUN0 + 硬盘 LUN1（96MB 原始镜像），
分别验证安装、从盘启动、文件持久。

## 已知限制 / 后续

- 文件系统格式化上限 32768 块（128MB），用户 1GB 磁盘只会用前 128MB —— 够装系统，后续可扩。
- 写盘是 4KB/请求同步方式，安装 96MB 盘约 1~2 分钟；可后续做多请求并发优化。
- 老的 virtio-blk 路径未动（`vb_*` 包装层 `vb_ok ? 本地 : vs_*`），veblk 虚拟机行为不变。
