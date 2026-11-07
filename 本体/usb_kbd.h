#ifndef USB_KBD_H
#define USB_KBD_H

/* LoongOS 阶段6①：xHCI + USB HID boot 键盘
 * usb_kbd_init()  —— 开机初始化（找不到设备时返回 -1，不影响串口 shell）
 * usb_kbd_poll()  —— 主循环调用：收割事件环、把按键字符塞进 kernel 的 ring
 */
int  usb_kbd_init(void);
void usb_kbd_poll(void);

#endif
