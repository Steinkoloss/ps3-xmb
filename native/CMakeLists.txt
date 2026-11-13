cmake_minimum_required(VERSION 3.25)
project(MoyouNative LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.9 REQUIRED COMPONENTS Gui OpenGL)
qt_standard_project_setup(REQUIRES 6.9)
# Everything also links into the Qt Quick plugin.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(MOYOU_WARNINGS -Wall -Wextra -Wpedantic -ffp-contract=off -fno-fast-math)
add_library(moyou-render STATIC native/GlProgram.cpp native/Renderer.cpp)
target_include_directories(moyou-render PUBLIC native)
target_link_libraries(moyou-render PUBLIC Qt6::Gui Qt6::OpenGL)
target_compile_options(moyou-render PRIVATE ${MOYOU_WARNINGS})
qt_add_executable(moyou-preview native/main.cpp)
# Multi-monitor layout check (native/DesktopLayout.h); run by tests/test_desktop_layout.py.
qt_add_executable(moyou-desktop-layout-test plasma/desktop_layout_test.cpp)
target_include_directories(moyou-desktop-layout-test PRIVATE native)
target_link_libraries(moyou-desktop-layout-test PRIVATE Qt6::Core)
target_link_libraries(moyou-preview PRIVATE moyou-render)
target_compile_options(moyou-preview PRIVATE ${MOYOU_WARNINGS})

# Guest translations keep strict IEEE flags at every level; -O0 is the reference build.
# x86-64-v2 enables ps3recomp's SSE4.1 shuffles without FMA/AVX code generation.
set(MOYOU_GUEST_OPTIMIZATION "-O2;-march=x86-64-v3" CACHE STRING "Optimization flags for original-code translations")
# LTO inlines the per-instruction SPU checks into the translations.
if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT MOYOU_IPO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${MOYOU_IPO})
endif()

# The renderer is native code. The private translations of the original programs are only
# needed to compare against them (MOYOU_VERIFY_NATIVE, *Original paths); without them the
# build uses no firmware-derived code at all.
set(MOYOU_CPU_DIRECTORY "" CACHE PATH "Optional: private original CPU translations from lift_rope.py")
if(MOYOU_CPU_DIRECTORY)
    add_library(moyou-rope STATIC native/RopeSolver.cpp
        ${MOYOU_CPU_DIRECTORY}/moyou_rope_program.cpp ${MOYOU_CPU_DIRECTORY}/moyou_math_program.cpp)
    target_include_directories(moyou-rope PRIVATE native ${MOYOU_CPU_DIRECTORY})
    target_compile_definitions(moyou-rope PRIVATE MOYOU_ORIGINAL_CPU PPU_NO_INLINE_LOADS PPU_NO_INLINE_STORES)
    target_compile_options(moyou-rope PRIVATE ${MOYOU_GUEST_OPTIMIZATION} -ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing -fno-semantic-interposition)
    qt_add_executable(moyou-rope-checkpoint native/rope_main.cpp)
    target_link_libraries(moyou-rope-checkpoint PRIVATE moyou-rope Qt6::Gui)
else()
    add_library(moyou-rope STATIC native/RopeSolver.cpp)
    target_include_directories(moyou-rope PRIVATE native)
    target_compile_options(moyou-rope PRIVATE -O2 -ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing -fno-semantic-interposition)
endif()

set(MOYOU_SPU_DIRECTORY "" CACHE PATH "Optional: private original SPU translations from lift_simulation.py")
set(MOYOU_RECOMPILER_DIRECTORY "" CACHE PATH "Pinned ps3recomp runtime headers (with MOYOU_SPU_DIRECTORY)")
add_library(moyou-surface STATIC native/SurfaceEvaluator.cpp)
target_compile_options(moyou-surface PRIVATE -ffp-contract=off -fno-fast-math)
if(MOYOU_SPU_DIRECTORY)
    enable_language(C)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)   # the SPU replay checks its images
    add_library(moyou-spu STATIC native/spu_replay.c
        ${MOYOU_SPU_DIRECTORY}/spline/spu_recomp.c ${MOYOU_SPU_DIRECTORY}/particles/spu_recomp.c)
    target_include_directories(moyou-spu PRIVATE native ${MOYOU_SPU_DIRECTORY}
        ${MOYOU_RECOMPILER_DIRECTORY}/runtime ${MOYOU_RECOMPILER_DIRECTORY}/include)
    target_compile_definitions(moyou-spu PRIVATE MOYOU_SPU_LIBRARY SPU_SCALAR_HELPERS SPU_SCALAR_INT)
    target_compile_options(moyou-spu PRIVATE ${MOYOU_GUEST_OPTIMIZATION} -ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing)
    target_link_libraries(moyou-spu PRIVATE OpenSSL::Crypto m)
    target_link_libraries(moyou-surface PRIVATE moyou-spu)
    target_compile_definitions(moyou-surface PRIVATE MOYOU_ORIGINAL_SPU)
    qt_add_executable(moyou-surface-checkpoint native/surface_main.cpp)
    target_link_libraries(moyou-surface-checkpoint PRIVATE moyou-surface Qt6::Core)
endif()

add_library(moyou-scene STATIC native/OriginalScene.cpp native/OriginalSimulation.cpp)
target_link_libraries(moyou-scene PUBLIC moyou-render moyou-rope moyou-surface)
target_compile_options(moyou-scene PRIVATE ${MOYOU_WARNINGS})
target_link_libraries(moyou-preview PRIVATE moyou-scene)
target_compile_definitions(moyou-preview PRIVATE MOYOU_HAVE_LIVE_SCENE)

# QML module PS3Xmb (XmbBackground) for the Plasma wallpaper; see plasma/install.sh.
find_package(Qt6 6.9 COMPONENTS Quick)
if(Qt6Quick_FOUND)
    qt_add_qml_module(moyou-qml URI PS3Xmb VERSION 1.0 PLUGIN_TARGET moyou-qml
        OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml/PS3Xmb SOURCES plasma/XmbItem.cpp plasma/XmbItem.h plasma/XmbSettings.cpp plasma/XmbSettings.h)
    target_include_directories(moyou-qml PRIVATE plasma)
    target_link_libraries(moyou-qml PRIVATE Qt6::Quick moyou-scene)
    target_compile_options(moyou-qml PRIVATE ${MOYOU_WARNINGS})
endif()

# Standalone app (Windows: behind the desktop icons; elsewhere: a plain window); see windows/.
find_package(Qt6 6.9 COMPONENTS Widgets)
if(Qt6Widgets_FOUND)
    qt_add_executable(ps3-xmb WIN32 windows/main.cpp)
    target_link_libraries(ps3-xmb PRIVATE moyou-scene Qt6::Widgets)
    target_compile_options(ps3-xmb PRIVATE ${MOYOU_WARNINGS})
endif()
