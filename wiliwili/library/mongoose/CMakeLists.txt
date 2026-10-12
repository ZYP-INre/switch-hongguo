cmake_minimum_required(VERSION 3.15)

option(MG_ENABLE_IPV6 "Enable IPv6 support" OFF)

set(MG_CUSTOM_COMPILE_FLAGS)

add_library(mongoose STATIC ${CMAKE_CURRENT_SOURCE_DIR}/mongoose.c)

if (__SWITCH__)
    list(APPEND MG_CUSTOM_COMPILE_FLAGS -D__SWITCH__)
    list(APPEND MG_CUSTOM_COMPILE_FLAGS -DMG_ARCH=10086)
endif ()

if (VITASDK)
    list(APPEND MG_CUSTOM_COMPILE_FLAGS -D__PSV_)
    list(APPEND MG_CUSTOM_COMPILE_FLAGS -DMG_ARCH=10010)
endif ()

if (MG_ENABLE_IPV6)
    message(STATUS "mongoose: Enabling IPv6 support")
    list(APPEND MG_CUSTOM_COMPILE_FLAGS -DMG_ENABLE_IPV6)
endif ()

target_compile_options(mongoose PUBLIC ${MG_CUSTOM_COMPILE_FLAGS})
target_include_directories(mongoose PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})