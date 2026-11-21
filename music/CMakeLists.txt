# music: 音乐播放器（48 kHz WAV，ES8311 输出）
# 硬件：Waveshare ESP32-S3-Touch-AMOLED-1.75C (ESP32-S3R8 / 466x466 AMOLED)
cmake_minimum_required(VERSION 3.16)

# 共享组件（boot_switch：长按 BOOT 回 selector、启动失败自动回退）
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 版本号会写进 esp_app_desc，selector 列表里显示的就是它
project(music VERSION 0.1.0)
