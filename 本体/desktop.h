/* LoongOS 桌面路线 P1 · 桌面外壳接口 */
#ifndef LOONGOS_DESKTOP_H
#define LOONGOS_DESKTOP_H

/* 进入图形桌面（阻塞，直到按 q / Esc 返回串口 shell）。
   没有帧缓冲时立即返回并打印提示，不破坏原有 shell。 */
void desktop_run(void);

#endif
