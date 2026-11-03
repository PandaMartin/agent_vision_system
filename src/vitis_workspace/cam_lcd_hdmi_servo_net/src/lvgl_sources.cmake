# =============================================================================
# LVGL 9.4库源与字体注入(由UserConfig.cmake尾部include引入)
#
# 放置史(教训):
#   1. 曾在UserConfig.cmake的USER SETTINGS段——Vitis IDE源管理重写该段并把
#      ${VAR}展开替换成字面量, LVGL整库从链接里消失过两次;
#   2. 后迁至CMakeLists.txt——2026-10起IDE retarget又把该文件整文件清空。
# 本文件IDE不认识、不重写; include发生在add_executable之前, GLOB结果追加进
# USER_COMPILE_SOURCES即被模板收进源清单。新增LVGL库文件/字体自动被GLOB
# 覆盖, 无需登记。
# =============================================================================
file(GLOB_RECURSE LVGL_SOURCES "${CMAKE_SOURCE_DIR}/sw/impl/lvgl/lvgl/src/*.c")
file(GLOB LVGL_FONTS "${CMAKE_SOURCE_DIR}/sw/impl/lvgl/fonts/*.c")
list(APPEND USER_COMPILE_SOURCES ${LVGL_SOURCES} ${LVGL_FONTS})
