# LOONGOS_HB_FIX — 后台心跳刷屏：只走串口不画屏幕
- [x] 1. 定位：task1_main 每 10 片（约 2 秒）打一行「我在干活」，uart_putc 会镜像到 GOP 帧缓冲 → 真机屏幕刷屏
- [x] 2. 修法：新增 hb_putc/hb_puts/hb_puthex（只写 UART_DR，不经过 fb_putc），task1 心跳改用 hb_*；串口保留心跳（测试/调试要用）
- [x] 3. 重编 + 重打 ISO
- [ ] 4. 全套回归 + TCG/KVM ISO 冒烟
- [ ] 5. NAS 归档 + 留痕
