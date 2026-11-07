# 灵珑系统 · 纯自研内核版

64 位 ARM 裸机内核操作系统，自带 shell、文件系统、用户程序、virtio-net 网络与 USB HID 键盘。含手机版(Termux + QEMU aarch64 虚拟机运行)与桌面系统架构路线。与 linglum-linux 是同一系统的两条技术路线。

> 本仓库由 NAS 归档包整理发布。已剥离运行时数据、密钥、审计日志与大型二进制镜像；
> 文档中的真实口令与内网 IP 已脱敏。镜像请用仓库内构建脚本生成，或从 Releases 获取。

## 目录结构

```
├── 手机版/
├── 本体/
├── 手机版/检查与修复记录_手机版.md
├── 手机版/说明.md
├── 本体/BOOTAA64.EFI
├── 本体/LOONGOS_DESKTOP_TODO.md
├── 本体/LOONGOS_FIXES_TODO.md
├── 本体/LOONGOS_HB_FIX_TODO.md
├── 本体/LOONGOS_INSTALL_TODO.md
├── 本体/LOONGOS_ISO_TODO.md
├── 本体/LOONGOS_KBD_TODO.md
├── 本体/LOONGOS_KVM_FIX_TODO.md
```

## 使用

请参考仓库内的说明文档与构建/部署脚本；首次运行前把示例地址、口令改成自己的值。

## 安全约定

- `data/`、`.env`、`env.*`、`*.pem`、`*.keystore`、`config.json`、`*.db`、`*.log` 均已在 `.gitignore` 中排除，切勿强制提交。
- 文档里的 `192.168.1.100` 与 `203.0.113.x` 是示例地址，`<REDACTED_PASSWORD>` 是脱敏占位符。
- 涉及管理后台的服务请只在可信局域网内使用。

## 许可证

MIT
