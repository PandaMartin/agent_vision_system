# rebuild_fpga.tcl —— 从仓库根批量重建 PL 实现
# 用法: vivado -mode batch -source build/rebuild_fpga.tcl -tclargs <仓库根目录>
# 说明: ① 将 ip_repo 指向工程内 ip_repo; ② 打开工程并升级 IP; ③ 综合+实现+写比特流。
#       本脚本按标准 Vivado Tcl 流程编写, 首次使用请先在有监督环境跑通一遍。
if {$argc < 1} {
    puts "用法: vivado -mode batch -source build/rebuild_fpga.tcl -tclargs <仓库根目录>"
    exit 1
}
set root [file normalize [lindex $argv 0]]
# 工程可位于仓库根、根下 18_cam_lcd_hdmi_servo_net 或 src/vivado_project/18_cam_lcd_hdmi_servo_net
set xpr ""
foreach cand [list $root \
               [file join $root 18_cam_lcd_hdmi_servo_net] \
               [file join $root src vivado_project 18_cam_lcd_hdmi_servo_net]] {
    set xpr [lindex [glob -nocomplain -directory $cand *.xpr] 0]
    if {$xpr ne ""} { break }
}
if {$xpr eq ""} { puts "错误: 未找到 .xpr"; exit 1 }
set proj_dir [file dirname $xpr]
set repo [file join $proj_dir ip_repo]
if {![file exists $repo]} { set repo [file join $root ip_repo] }

open_project $xpr
set_property ip_repo_paths [list $repo] [current_project]
update_ip_catalog

# 重新生成 BD 输出产物
set bdf [file join $proj_dir [lindex [lsort [glob -nocomplain -directory $proj_dir *.srcs]] 0] sources_1 bd system system.bd]
if {[catch {open_bd_design $bdf} err]} { puts "警告: 打开 BD 失败: $err"; puts "请确认 ip_repo 完整后重试。" }
upgrade_ip [get_ips]

reset_run synth_1
launch_runs impl_1 -to_step write_bitstream
wait_on_run impl_1
puts "完成。实现报告见 .runs/impl_1/。"
