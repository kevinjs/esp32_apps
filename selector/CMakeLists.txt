cmake_minimum_required(VERSION 3.16)

# 共享组件（boot_switch）
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(selector)
