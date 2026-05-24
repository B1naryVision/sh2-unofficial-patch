cmake_minimum_required(VERSION 3.10)
project(S2_UnofficialPatch CXX)

if(MSVC)
    add_compile_options(/arch:IA32)
    add_link_options(/MACHINE:X86)
else()
    add_compile_options(-m32)
    add_link_options(-m32)
endif()

add_library(S2_UnofficialPatch SHARED
    src/dllmain.cpp
    src/core/hook.cpp
    src/core/log.cpp
    src/proxy/version_proxy.cpp
    src/patches/registry.cpp
    src/patches/knight_catapult_crash.cpp
)

target_include_directories(S2_UnofficialPatch PRIVATE src)
target_link_libraries(S2_UnofficialPatch PRIVATE kernel32)
