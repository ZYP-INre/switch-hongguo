cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif ()
set(DEVKITPRO $ENV{DEVKITPRO} CACHE BOOL "DEVKITPRO")
include(${DEVKITPRO}/cmake/Switch.cmake REQUIRED)
set(ARCH "-march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE")
set(CMAKE_C_FLAGS "${ARCH} -g -Wall -O2 -ffunction-sections")
set(CMAKE_ASM_FLAGS "-g ${ARCH}")
set(CMAKE_EXE_LINKER_FLAGS "-L${DEVKITPRO}/libnx/lib -specs=${DEVKITPRO}/libnx/switch.specs -g ${ARCH}")
enable_language(ASM)

find_program(HACBREWPACK_EXE NAMES hacbrewpack)
if (NOT HACBREWPACK_EXE)
	message(FATAL_ERROR "Could not find hacbrewpack: try installing https://github.com/The-4n/hacBrewPack")
endif()

project(nsp_forwarder)

# ⚠ 这两个名字决定「装到主页」后图标上显示什么。
#   ${PROJECT_NAME} 在这个子项目里是 nsp_forwarder —— 直接拿去当显示名的话，
#   装到主界面后图标上写的就是 "nsp_forwarder"。
#   APP_DISPLAY_TITLE / APP_DISPLAY_AUTHOR / APP_OUTPUT_NAME 由父 CMakeLists
#   定义并被子作用域继承；父里没定义时（单独构建本子项目）才回落到默认。
if (NOT DEFINED APP_DISPLAY_TITLE)
    set(APP_DISPLAY_TITLE ${PROJECT_NAME})
endif ()
if (NOT DEFINED APP_DISPLAY_AUTHOR)
    set(APP_DISPLAY_AUTHOR ${PROJECT_AUTHOR})
endif ()
if (NOT DEFINED APP_OUTPUT_NAME)
    set(APP_OUTPUT_NAME ${PROJECT_NAME})
endif ()
message(STATUS "forwarder 元信息: title=[${APP_DISPLAY_TITLE}] author=[${APP_DISPLAY_AUTHOR}] out=[${APP_OUTPUT_NAME}] PROJECT_NAME=[${PROJECT_NAME}]")
set(PROJECT_TITLEID "010ff000ffff0001")
set(FORWARDER_SRC "${PROJECT_SOURCE_DIR}/source/main.c" "${PROJECT_SOURCE_DIR}/source/trampoline.s")
add_executable(${PROJECT_NAME} ${FORWARDER_SRC})
target_link_libraries(${PROJECT_NAME} PRIVATE nx)
target_compile_definitions(${PROJECT_NAME} PRIVATE VERSION="2.4.3")
# 让 forwarder 去启动我们的 .nro，而不是写死的 wiliwili.nro
target_compile_definitions(${PROJECT_NAME} PRIVATE
    HG_NRO_FILE="${APP_OUTPUT_NAME}.nro"
    HG_NRO_DIR="${APP_OUTPUT_NAME}")

if (NOT PROJECT_KEYSET)
    SET(PROJECT_KEYSET "${PROJECT_BINARY_DIR}/prod.keys")
    file(DOWNLOAD https://raw.githubusercontent.com/externalEmu/s/master/prod.keys ${PROJECT_KEYSET})
endif ()

add_custom_target(${PROJECT_NAME}.pfs0 DEPENDS ${PROJECT_NAME}
    COMMAND ${CMAKE_COMMAND} -E make_directory "exefs"
    COMMAND ${NX_ELF2NSO_EXE} "${PROJECT_NAME}.elf" "exefs/main"
    COMMAND ${NX_NPDMTOOL_EXE} "${PROJECT_SOURCE_DIR}/wiliwili.json" "exefs/main.npdm"
)

add_custom_target(${PROJECT_NAME}.nsp DEPENDS ${PROJECT_NAME}.pfs0
    COMMAND ${CMAKE_COMMAND} -E make_directory "control" "logo"
    COMMAND ${NX_NACPTOOL_EXE} --create "${APP_DISPLAY_TITLE}" "${APP_DISPLAY_AUTHOR}" "${APP_VERSION}" "control/control.nacp"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_RESOURCES}/icon/logo.png" "logo/NintendoLogo.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_RESOURCES}/icon/logo.gif" "logo/StartupMovie.gif"
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_ICON}" "control/icon_AmericanEnglish.dat"
    # ⚠ --titlename 必须用显示名，不能用 APP_OUTPUT_NAME。
    #   实测：这里传 hongguo 的话，hacbrewpack 会把 nacptool 刚生成的
    #   中文标题覆盖掉，最终装到主页的图标上写的是 hongguo。
    COMMAND ${HACBREWPACK_EXE} -k "${PROJECT_KEYSET}" --titleid ${PROJECT_TITLEID} --titlename "${APP_DISPLAY_TITLE}" --noromfs --nostartupuser
    COMMAND ${CMAKE_COMMAND} -E rename "hacbrewpack_nsp/${PROJECT_TITLEID}.nsp" "${PROJECT_RESOURCES}/${PROJECT_NAME}.nsp"
)