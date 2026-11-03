# run_sim.tcl —— ov5640_capture_data RTL 功能仿真一键脚本（Vivado xsim）
# 用法: vivado -mode batch -source run_sim.tcl
# 说明: 在独立工程中仿真自定义采集 IP，不触碰原 Vivado 工程；
#       结果打印 CHECK/TB_PASS 行，完整日志见 build/.../xsim/simulate.log
set script_dir [file dirname [file normalize [info script]]]
set ip_src     "$script_dir/../ip_repo/ov5640_cap_data/SRC/ov5640_capture_data.v"
set tb_src     "$script_dir/tb_ov5640_capture_data.v"

if {![file exists $ip_src]} { puts "ERROR: IP 源不存在: $ip_src"; exit 1 }

create_project -force ov5640_cap_sim "$script_dir/build"
add_files $ip_src
add_files -fileset sim_1 $tb_src
set_property top ov5640_capture_data [current_fileset]
set_property top tb_ov5640_capture_data [get_filesets sim_1]

# run all + 由测试平台 $finish 收尾
set fh [open "$script_dir/xsim_run.tcl" w]
puts $fh "open_vcd tb.vcd"
puts $fh "log_vcd -recursive *"
puts $fh "run all"
puts $fh "close_vcd"
puts $fh "quit"
close $fh
set_property -name {xsim.simulate.runtime} -value {all} -objects [get_filesets sim_1]
set_property -name {xsim.simulate.tclbatch} -value "$script_dir/xsim_run.tcl" -objects [get_filesets sim_1]

launch_simulation
puts "SIM_DONE"
