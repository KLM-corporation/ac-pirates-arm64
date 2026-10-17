cmake_minimum_required(VERSION 3.18)
project(acp64 C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dynarmic (ARM32 guest only).
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(Boost_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/boost CACHE PATH "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

set(ZB_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function)

# The Lua chunks run by the cheat codes, embedded as strings.
file(READ ${CMAKE_SOURCE_DIR}/tools/lua/cheat_infinite.lua ZB_CHEAT_LUA)
file(READ ${CMAKE_SOURCE_DIR}/tools/lua/unlock_queen.lua ZB_QUEEN_LUA)
configure_file(${CMAKE_SOURCE_DIR}/src/cheat/cheat_lua.h.in ${CMAKE_BINARY_DIR}/generated/cheat_lua.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/tools/lua/cheat_infinite.lua ${CMAKE_SOURCE_DIR}/tools/lua/unlock_queen.lua)

add_library(zbcore STATIC
    src/log.cpp
    src/mem.cpp
    src/dlmalloc_glue.c
    src/cpu.cpp
    src/elf_loader.cpp
    src/runtime.cpp
    src/cheat/cheat.cpp
    src/game/perf_patches.cpp
    src/hle/hle_libc.cpp
    src/hle/hle_math.cpp
    src/hle/hle_stdio.cpp
    src/hle/hle_pthread.cpp
    src/hle/hle_posix.cpp
    src/hle/hle_time.cpp
    src/hle/hle_net.cpp
    src/hle/hle_dl.cpp
    src/hle/hle_log.cpp
)
target_compile_options(zbcore PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${ZB_WARNINGS}>)
set_source_files_properties(src/dlmalloc_glue.c PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(zbcore PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(zbcore PUBLIC dynarmic log android EGL GLESv2 OpenSLES z dl m)

add_executable(zbtest src/tools/zbtest.cpp)
target_link_libraries(zbtest PRIVATE zbcore)
target_link_options(zbtest PRIVATE -Wl,-z,max-page-size=16384)

# The replacement libSparkApp.so loaded by NativeActivity.
add_library(SparkApp SHARED
    src/android/entry.cpp
    src/android/android_glue.cpp
    src/android/cheat_ui.cpp
    src/android/egl_gles.cpp
    src/android/opensl.cpp
    src/jni/jni_bridge.cpp
    src/jni/natives_gen.cpp
)
target_compile_options(SparkApp PRIVATE ${ZB_WARNINGS})
target_link_libraries(SparkApp PRIVATE zbcore)
target_link_options(SparkApp PRIVATE
    -Wl,-z,max-page-size=16384
    -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/android/exports.map
    -Wl,--gc-sections)
