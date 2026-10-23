cmake_minimum_required(VERSION 3.25)
project(ReRevvedRawAdapter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(thirdparty/fmt)

add_executable(raw_adapter
    XenosRecomp/raw_adapter.cpp
    XenosRecomp/shader_recompiler.cpp
)

target_include_directories(raw_adapter PRIVATE
    XenosRecomp
    thirdparty/dxc-bin/inc
    thirdparty/xxHash
    thirdparty/zstd/lib
    thirdparty/smol-v/source
)

target_precompile_headers(raw_adapter PRIVATE XenosRecomp/pch.h)
target_compile_definitions(raw_adapter PRIVATE REBLUE_RECOMP _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(raw_adapter PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
target_link_libraries(raw_adapter PRIVATE fmt::fmt)
