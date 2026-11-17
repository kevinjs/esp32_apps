# ride-gmeter: 骑行加速度指示器
# 硬件：Waveshare ESP32-S3-Touch-AMOLED-1.75C (ESP32-S3R8 / 466x466 AMOLED)
cmake_minimum_required(VERSION 3.16)

# 共享组件（boot_switch：长按 BOOT 返回 selector、启动失败自动回退）
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ride-gmeter)
