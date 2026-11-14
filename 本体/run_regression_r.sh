#!/bin/sh
# R 轮「系统资源调配」全套回归（含真实画面判据，杜绝串口假绿）
# 用法：sh run_regression_r.sh   —— 每项超时 400s，日志写 /tmp/reg_r_*.log
cd "$(dirname "$0")" || exit 1
i=0; pass=0; fail=0
run(){
  i=$((i+1)); name="$1"; shift
  LOG="/tmp/reg_r_${i}.log"
  timeout 400 "$@" > "$LOG" 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then echo "[$i] PASS  $name"; pass=$((pass+1));
  else echo "[$i] FAIL($rc)  $name  -> $LOG"; fail=$((fail+1)); fi
}
echo "########## 全套回归（15 项：资源调配 + ARM 长稳 + 网络 + 安装）##########"
run "真实画面判据(800x600非黑屏)"   python3 test_framebuffer.py
run "行为黄金基线"                 python3 test_behavior.py --compare golden_behavior.txt
run "NVMe 装盘+持久化"             python3 test_nvme_install.py
run "UEFI 5 拓扑"                  sh test_uefi.sh
run "桌面 P2 端到端"               python3 test_desktop_p2.py
run "EL0 用户程序 33 用例"         python3 test_stage5b_elf.py
run "鼠标跟随 P2.2"                python3 test_mousemove.py
run "鼠标校准 P2.1"                python3 test_mousecal.py
run "直启驱动识别"                 sh test_drivers.sh
run "scsi 装盘"                    python3 test_scsi_install.py
run "P3 手感(交付版)"              python3 test_desktop_p3.py
run "资源在线验证(画面+wfi/tick/调频)" python3 test_resource_live.py
run "ARM 内核长稳压力(异常/时钟/洪水)" python3 test_stress_arm.py
run "8 网卡多型号逐卡验收"            python3 test_nic.py
run "多盘安装+选盘+已装系统菜单"      python3 test_install_disk.py
echo "########## 汇总: PASS=$pass FAIL=$fail ##########"
[ "$fail" -eq 0 ]
