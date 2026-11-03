# Copyright (C) 2023 - 2025 Advanced Micro Devices, Inc.  All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
enable_language(C ASM CXX)

###    USER SETTINGS  START    ###
# Below settings can be customized
# User needs to edit it manually as per their needs.
###    DO NOT ADD OR REMOVE VARIABLES FROM THIS SECTION    ###
# -----------------------------------------
# Add any compiler definitions, they will be added as extra definitions
# Example : Adding VERBOSE=1 will pass -DVERBOSE=1 to the compiler.
set(USER_COMPILE_DEFINITIONS
"__INT32_TYPE__=int"
)

# Undefine any previously specified compiler definitions, either built in or provided with a -D option
# Example : Adding MY_SYMBOL will pass -UMY_SYMBOL to the compiler.
set(USER_UNDEFINED_SYMBOLS
"__clang__"
)


# Add any directories below, they will be added as include directories.
# Example 1: Adding /proj/data/include will pass -I/proj/data/include.
# Example 2: Adding ../../common/include will consider the path as relative to this component directory.
# Example 3: Adding ${CMAKE_SOURCE_DIR}/data/include to add data/include from this project.

set(USER_INCLUDE_DIRECTORIES
"${CMAKE_SOURCE_DIR}/sw/impl/nn/ncnn_inc"
"${CMAKE_SOURCE_DIR}/sw/impl/nn/cxxshim"
"${CMAKE_SOURCE_DIR}/sw/impl/lvgl"
"${CMAKE_SOURCE_DIR}/sw/impl/lvgl/lvgl"
)

#Add any source below, they will be added as Compile sources.
#Example 1: Adding /proj/data/include/helloworld.c would pass -I/proj/data/include to the compiler.
#Example 2: Adding ../../common/helloworld.c will consider the path as relative to this component directory
#Example 3: Adding ${MY_ENV}/helloworld.c are expanded using project-specific environment settings.
# =============================================================================
# 四层架构源清单(详见各层README/注释):
#   hw/     板级支持: bus(总线) -> ip(IP驱动) -> dev(器件) -> chain(链路编排)
#   sw/impl 实现层(纯逻辑, GUI路径经port.h触碰平台): port/sys/sd/lcd/cam/nn/lvgl/net
#   sw/app  固件应用系统: 命令总线/数据桥/界面壳(launcher)
#           int_app/ 内置应用(一应用一.c/.h) ext_app/ SD应用加载框架
# LVGL 9.4库源: 由本文件尾部include的lvgl_sources.cmake注入(本文件只放
# 字面量清单——Vitis IDE的源管理会重写USER SETTINGS段, ${VAR}展开放这里
# 会被删, LVGL整库丢过两次; CMakeLists.txt也会被IDE retarget整文件清空)
# =============================================================================

set(USER_COMPILE_SOURCES
"sw/impl/lcd/ovl_draw.c"
"hw/dev/servo.c"
"hw/bus/emio_gpio.c"
"hw/bus/gpio_i2c.c"
"hw/ip/vdma.c"
"hw/ip/vtc_api.c"
"hw/ip/clk_wiz.c"
"hw/ip/lcd_modes.c"
"hw/ip/axi_gpio.c"
"hw/dev/lcd_panel.c"
"hw/dev/ov5640.c"
"hw/dev/touch.c"
"hw/dev/gt9147.c"
"hw/dev/gt9271.c"
"hw/dev/ft5206.c"
"hw/chain/lcd_chain.c"
"hw/chain/cam_chain.c"
"sw/impl/port/port_board.c"
"sw/impl/sys/platform.cpp"
"sw/impl/sd/sd.cpp"
"sw/impl/sd/photo.cpp"
"sw/impl/lcd/lcd.c"
"sw/impl/lcd/lvgl_port.c"
"sw/impl/lcd/vidwin.c"
"sw/impl/cam/cam.c"
"sw/impl/nn/nn_runner.cpp"
"sw/impl/nn/nn_module.c"
"sw/impl/nn/model_mgr.cpp"
"sw/impl/nn/nn_selftest.cpp"
"sw/impl/nn/nn_wdt.c"
"sw/impl/net/net_server.c"
"sw/app/appctl.c"
"sw/app/bridge.c"
"sw/app/launcher.c"
"sw/app/ext_app/loader.c"
"sw/app/ext_app/sdapp.c"
"sw/app/int_app/app_camera.c"
"sw/app/int_app/app_infer.c"
"sw/app/int_app/app_gimbal.c"
"sw/app/int_app/app_files.c"
"sw/app/int_app/app_transfer.c"
"sw/app/int_app/app_test.c"
"main.cpp"
)

# -----------------------------------------

# Turn on all optional warnings (-Wall)
set(USER_COMPILE_WARNINGS_ALL -Wall)

# Enable extra warning flags (-Wextra)
set(USER_COMPILE_WARNINGS_EXTRA -Wextra)

# Make all warnings into hard errors (-Werror)
set(USER_COMPILE_WARNINGS_AS_ERRORS )

# Check the code for syntax errors, but don't do anything beyond that (-fsyntax-only)
set(USER_COMPILE_WARNINGS_CHECK_SYNTAX_ONLY )

# Issue all the mandatory diagnostics listed in the C standard (-pedantic)
set(USER_COMPILE_WARNINGS_PEDANTIC )

# Issue all the mandatory diagnostics, and make all mandatory diagnostics into errors. (-pedantic-errors)
set(USER_COMPILE_WARNINGS_PEDANTIC_AS_ERRORS )

# Suppress all warnings (-w)
set(USER_COMPILE_WARNINGS_INHIBIT_ALL )

# -----------------------------------------

# Optimization level   "-O0" [None], "-O1" [Optimize] , "-O2" [Optimize More], "-O3" [Optimize for Size] or "-Os" [Optimize for Size]
set(USER_COMPILE_OPTIMIZATION_LEVEL -O2)

# Other flags related to optimization
set(USER_COMPILE_OPTIMIZATION_OTHER_FLAGS )

# -----------------------------------------

# Debug level "" [None], "-g1" [Minimum], "g2" [Default], "-g3" [Maximum]
set(USER_COMPILE_DEBUG_LEVEL -g3)

# Other flags related to debugging
set(USER_COMPILE_DEBUG_OTHER_FLAGS )

# -----------------------------------------

# Enable profiling (-pg) (This feature is not supported currently)
# set(USER_COMPILE_PROFILING_ENABLE )

# -----------------------------------------

# Verbose (-v)
set(USER_COMPILE_VERBOSE )

# Support ANSI_PROGRAM (-ansi)
set(USER_COMPILE_ANSI )
set(USER_COMPILE_RELAXATION "-Wl,--no-relax")
set(USER_COMPILE_GARBAGE "")
# Add any compiler options that is not covered by the above variables, they will be added as extra compiler options
# To enable profiling -pg [ for gprof ]  or -p [ for prof ]
set(USER_COMPILE_OTHER_FLAGS "-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard")

# -----------------------------------------

# Linker options
# Do not use the standard system startup files when linking.
# The standard system libraries are used normally, unless -nostdlib or -nodefaultlibs is used. (-nostartfiles)
set(USER_LINK_NO_START_FILES )

# Do not use the standard system libraries when linking. (-nodefaultlibs)
set(USER_LINK_NO_DEFAULT_LIBS )

# Do not use the standard system startup files or libraries when linking. (-nostdlib)
set(USER_LINK_NO_STDLIB )

# Omit all symbol information. (-s)
set(USER_LINK_OMIT_ALL_SYMBOL_INFO )


# -----------------------------------------

# Add any libraries to be linked below, they will be added as extra libraries.
# User needs to update USER_LINK_DIRECTORIES below with their library search paths.
set(USER_LINK_LIBRARIES
"ncnn"
)

# Add any directories to look for the libraries to be linked.
# Example 1: Adding /proj/compression/lib will pass -L/proj/compression/lib to the linker.
# Example 2: Adding ../../common/lib will consider the path as relative to this directory and will pass -L option.
set(USER_LINK_DIRECTORIES
"${CMAKE_SOURCE_DIR}/sw/impl/nn/ncnn_lib"
)

# -----------------------------------------

set(USER_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/lscript.ld")

# Add linker options to be passed to the linker they will be added as extra linker options
# Example : Adding -s will pass -s to the linker.
set(USER_LINK_OTHER_FLAGS "-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard"
)

# -----------------------------------------

###   END OF USER SETTINGS SECTION ###
###   DO NOT EDIT BEYOND THIS LINE ###

set(USER_COMPILE_OPTIONS
    " ${USER_COMPILE_WARNINGS_ALL}"
    " ${USER_COMPILE_WARNINGS_EXTRA}"
    " ${USER_COMPILE_WARNINGS_AS_ERRORS}"
    " ${USER_COMPILE_WARNINGS_CHECK_SYNTAX_ONLY}"
    " ${USER_COMPILE_WARNINGS_PEDANTIC}"
    " ${USER_COMPILE_WARNINGS_PEDANTIC_AS_ERRORS}"
    " ${USER_COMPILE_WARNINGS_INHIBIT_ALL}"
    " ${USER_COMPILE_OPTIMIZATION_LEVEL}"
    " ${USER_COMPILE_OPTIMIZATION_OTHER_FLAGS}"
    " ${USER_COMPILE_DEBUG_LEVEL}"
    " ${USER_COMPILE_DEBUG_OTHER_FLAGS}"
    " ${USER_COMPILE_VERBOSE}"
    " ${USER_COMPILE_ANSI}"
    " ${USER_COMPILE_OTHER_FLAGS}"
)
foreach(entry ${USER_UNDEFINED_SYMBOLS})
    list(APPEND USER_COMPILE_OPTIONS " -U${entry}")
endforeach()

# Process USER_LINK_DIRECTORIES to generate proper -L flags
if(USER_LINK_DIRECTORIES)
    # Convert list to string with proper -L flag formatting
    string(REPLACE ";" "\" -L\"" _formatted_dirs "${USER_LINK_DIRECTORIES}")
    set(USER_LINK_DIRECTORIES "${_formatted_dirs}")
endif()

set(USER_LINK_OPTIONS
    " ${USER_LINKER_NO_START_FILES}"
    " ${USER_LINKER_NO_DEFAULT_LIBS}"
    " ${USER_LINKER_NO_STDLIB}"
    " ${USER_LINKER_OMIT_ALL_SYMBOL_INFO}"
    " ${USER_LINK_OTHER_FLAGS}"
    #链接map: 符号导出工具(sdapp export)的输入。
    #--no-demangle: 修饰名才是合法标识符, 否则ncnn等C++符号无法进导出表
    #(模型模块经fw_exports链接调用ncnn原语, 见docs/模型平台化设计.md)
    " -Wl,--no-demangle"
    " -Wl,-Map=${CMAKE_SOURCE_DIR}/../build/fw.map"
)
if(("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "microblaze") OR ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "microblaze_riscv"))
	if(USER_COMPILE_RELAXATION)
		string(FIND "${CMAKE_C_LINK_FLAGS}" "-Wl,--no-relax" POSITION)
		if(POSITION EQUAL -1)
		    set(CMAKE_C_LINK_FLAGS "  -Wl,--no-relax ${CMAKE_C_LINK_FLAGS}" CACHE STRING "CMAKE C LINK FLAGS" FORCE)
		    set(CMAKE_ASM_LINK_FLAGS "  -Wl,--no-relax ${CMAKE_ASM_LINK_FLAGS}" CACHE STRING "CMAKE ASM LINK FLAGS" FORCE)
		    set(CMAKE_CXX_LINK_FLAGS "  -Wl,--no-relax ${CMAKE_CXX_LINK_FLAGS}" CACHE STRING "CMAKE CXX LINK FLAGS" FORCE)
		endif()
	else()
		string(REPLACE "-Wl,--no-relax" "" CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS}")
		string(REPLACE "-Wl,--no-relax" "" CMAKE_ASM_LINK_FLAGS "${CMAKE_ASM_LINK_FLAGS}")
		string(REPLACE "-Wl,--no-relax" "" CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS}")
	endif()
	if(USER_COMPILE_GARBAGE)
		string(FIND "${CMAKE_C_FLAGS}" "-ffunction-sections -fdata-sections" POSITION)
		if(POSITION EQUAL -1)
		    set(CMAKE_C_FLAGS " -ffunction-sections -fdata-sections ${CMAKE_C_FLAGS}" CACHE STRING "CMAKE C FLAGS" FORCE)
		    set(CMAKE_CXX_FLAGS " -ffunction-sections -fdata-sections ${CMAKE_CXX_FLAGS}" CACHE STRING "CMAKE CXX FLAGS" FORCE)
		    set(CMAKE_ASM_FLAGS " -ffunction-sections -fdata-sections ${CMAKE_ASM_FLAGS}" CACHE STRING "CMAKE ASM FLAGS" FORCE)
		endif()
	else()
		string(REPLACE "-ffunction-sections -fdata-sections" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
		string(REPLACE "-ffunction-sections -fdata-sections" "" CMAKE_CXX_FLAGS "${CMAKE_ASM_FLAGS}")
		string(REPLACE "-ffunction-sections -fdata-sections" "" CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS}")
	endif()
endif()

# ----------------------------------------------------------------------------
# ncnn integration (ported from fpga_ncnn vitis_ws/p6_real UserConfig.cmake)
#
# force the hard-float NEON ABI everywhere so all objects match libncnn.a
add_compile_options(-mcpu=cortex-a9;-mfpu=neon-vfpv3;-mfloat-abi=hard)
add_link_options(-mcpu=cortex-a9;-mfpu=neon-vfpv3;-mfloat-abi=hard)

# C++ translation units must not use libstdc++: ncnn is built with
# NCNN_SIMPLESTL which provides operator new/delete itself, and linking
# libstdc++ would duplicate them. -nostdinc++ removes the toolchain C++
# headers (the cxxshim include dir provides a fallback <new>);
# -fno-exceptions/-fno-rtti match the library build.
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
)

# keep the project type "c" so the generated CMakeLists.txt neither injects
# -lstdc++ nor compiles the .c driver files as C++
set(PROJECT_TYPE "c")

# after project() runs, strip the libstdc++ include dirs so the template's
# -isystem re-injection cannot bypass -nostdinc++
set(CMAKE_PROJECT_INCLUDE "${CMAKE_SOURCE_DIR}/ncnn_port_post_project.cmake")

# LVGL库源/字体注入(见lvgl_sources.cmake头部)。刻意放在END OF USER SETTINGS
# 标记线以下: IDE源管理只重写上方USER SETTINGS段, 本尾部自定义区(ncnn块
# 同理)历次重写均原样保留
include("${CMAKE_SOURCE_DIR}/lvgl_sources.cmake")
