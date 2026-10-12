# SPDX-License-Identifier: GPL-3.0-only
#
# hongguo-core —— 红果短剧内容层，C++17 静态库。
#
# 设计约束（见 docs/superpowers/specs/2026-10-11-hongguo-switch-content-layer-design.md）：
#   * 零 UI 依赖 —— 本文件不得出现 borealis / nanovg / mpv
#   * 密码学零外部依赖 —— 保证 macOS 与 Switch 上逐字节一致
#   * 唯一外部依赖是 libcurl（macOS 系统库 / Switch switch-curl）

cmake_minimum_required(VERSION 3.16)
project(hongguo_core LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

# 供 clangd / 编辑器解析头文件路径
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

file(GLOB HG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

find_package(CURL REQUIRED)

add_library(hongguo_core STATIC ${HG_SOURCES})

# ---- 命令行工具 ----

add_executable(hgctl "${CMAKE_CURRENT_SOURCE_DIR}/tools/hgctl.cpp")
target_link_libraries(hgctl PRIVATE hongguo_core)
find_package(Threads REQUIRED)
target_link_libraries(hongguo_core PUBLIC Threads::Threads)

target_include_directories(hongguo_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party")   # nlohmann/json
target_link_libraries(hongguo_core PUBLIC CURL::libcurl)

# ---- 测试 ----

option(HG_BUILD_TESTS "构建测试" ON)

if(HG_BUILD_TESTS)
  enable_testing()
  file(GLOB HG_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  add_executable(hongguo_tests ${HG_TEST_SOURCES})
  target_link_libraries(hongguo_tests PRIVATE hongguo_core)
  target_compile_definitions(hongguo_tests PRIVATE
    HG_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
  add_test(NAME hongguo_tests COMMAND hongguo_tests)
endif()
