/* LoongOS 桌面路线 P2 · 图形终端（gfxterm.h）
 *
 * 目标：「在图形页面上操作电脑的所有功能」。做法不是重写一套 GUI 版命令，
 * 而是把 kernel.c 的 shell_dispatch_line() 直接接进来 —— 命令清单、管道、
 * 重定向、权限、文件系统、网络、install 装盘，全部复用同一份实现，不产生分叉。
 *
 * 输出：uart_putc() 挂钩 gfxterm_putc()，所有命令输出自动进历史缓冲；
 *       串口输出照旧（自动化回归不受影响），屏幕文字镜像在桌面期间已被关掉。
 * 输入：desktop.c 把键盘字符交给 gfxterm_key()，回车提交；鼠标点的快捷命令
 *       走 gfxterm_exec()。
 */
#ifndef LOONGOS_GFXTERM_H
#define LOONGOS_GFXTERM_H

#include "gfx.h"

void gfxterm_init(void);                 /* 桌面启动时：清历史 + 写欢迎信息 + 开捕获 */
void gfxterm_stop(void);                 /* 桌面退出时：关捕获 */
void gfxterm_putc(char c);               /* uart_putc 挂钩（放最前面，\r 自动忽略） */
void gfxterm_key(char c);                /* 输入行编辑，回车执行命令 */
void gfxterm_exec(const char *line);     /* 直接执行一条命令（开始菜单 / 按钮用） */
void gfxterm_draw(struct gfx_rect r, int focused);   /* 画终端窗口内容区 */
int  gfxterm_dirty(void);                /* 有新输出/输入变化，返回 1 并清标志 */
void gfxterm_scroll(int delta);          /* delta>0 向上翻历史，<0 向下 */

#endif /* LOONGOS_GFXTERM_H */
