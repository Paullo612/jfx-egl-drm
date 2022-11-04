cmake_minimum_required (VERSION 3.24)

project(jfx-egl-drm C)

set(PRE_MULTIPLY_CURSOR "OFF" CACHE BOOL "Wether to pre-multiply cursor image before seting it to cursor plane")
set(SCALE_FACTOR "1." CACHE STRING "Scale factor to use")

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS EGL)
find_package(JNI REQUIRED)

pkg_check_modules(libdrm REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(libgbm REQUIRED IMPORTED_TARGET gbm)

add_library(${PROJECT_NAME} ./src/jfx-egl-drm.c)

target_link_libraries(${PROJECT_NAME} PUBLIC JNI::JNI PRIVATE OpenGL::EGL PkgConfig::libdrm PkgConfig::libgbm)

target_compile_definitions(${PROJECT_NAME} PRIVATE SCALE_FACTOR=${SCALE_FACTOR})

if (${PRE_MULTIPLY_CURSOR})
    target_compile_definitions(${PROJECT_NAME} PRIVATE PRE_MULTIPLY_CURSOR)
endif()
