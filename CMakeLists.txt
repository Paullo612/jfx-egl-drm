cmake_minimum_required (VERSION 3.24)

project(jfx-egl-drm C)

set(PRE_MULTIPLY_CURSOR "OFF" CACHE BOOL "Wether to pre-multiply cursor image before seting it to cursor plane")
set(SCALE_FACTOR "1." CACHE STRING "Scale factor to use")

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS EGL)
find_package(JNI REQUIRED)

pkg_check_modules (LIBDRM REQUIRED libdrm)
pkg_check_modules (LIBGBM REQUIRED gbm)

add_library(${PROJECT_NAME} ./src/jfx-egl-drm.c)

target_include_directories(${PROJECT_NAME} PRIVATE ${LIBDRM_INCLUDE_DIRS} ${LIBGBM_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} JNI::JNI OpenGL::EGL ${LIBGBM_LIBRARIES} ${LIBGBM_LIBRARIES})
target_compile_options(${PROJECT_NAME} PRIVATE ${LIBDRM_CFLAGS_OTHER} ${LIBGBM_CFLAGS_OTHER})

target_compile_definitions(${PROJECT_NAME} PRIVATE SCALE_FACTOR=${SCALE_FACTOR})

if (${PRE_MULTIPLY_CURSOR})
    target_compile_definitions(${PROJECT_NAME} PRIVATE PRE_MULTIPLY_CURSOR)
endif()
