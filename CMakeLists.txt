cmake_minimum_required(VERSION 3.14)

if(NOT DEFINED ARCH)
    set(ARCH "x64")
endif()

message(STATUS "-------------------------------------------------")
message(STATUS "BUILDING FOR ARCH: ${ARCH}")
message(STATUS "-------------------------------------------------")

# Cross-compilation settings
if(APPLE OR UNIX)
    set(CMAKE_SYSTEM_NAME Windows)
    
    if(ARCH STREQUAL "x86")
        set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
        set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
        set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
    else()
        set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
        set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
        set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
    endif()
    
    set(CMAKE_FIND_ROOT_PATH /opt/homebrew/Cellar/mingw-w64 /usr/local/Cellar/mingw-w64 /opt/homebrew /usr/local)
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    # Allow searching both the specified root paths and the host system paths for libraries/includes
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
endif()

project(FPSLimiter)

set(CMAKE_CXX_STANDARD 17)

# Add common Homebrew include/lib paths so cross-compile can find SDK headers/libs
include_directories(/opt/homebrew/include /usr/local/include)
link_directories(/opt/homebrew/lib /usr/local/lib ${CMAKE_SOURCE_DIR}/vulkan)

include(FetchContent)

# 1. MinHook
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# 2. ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
file(GLOB IMGUI_SOURCES 
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    # Vulkan backend is added only for x64 (requires vulkan import lib)
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

if(ARCH STREQUAL "x64")
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
endif()

# ----------------------------------------------------
# A. Launcher (FPSLauncher.exe)
# ----------------------------------------------------
add_executable(FPSLauncher src/launcher.cpp ${IMGUI_SOURCES})
target_include_directories(FPSLauncher PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(FPSLauncher PRIVATE comdlg32 shlwapi d3d11 dxgi d3dcompiler dwmapi winmm)
if(ARCH STREQUAL "x64")
    target_link_libraries(FPSLauncher PRIVATE vulkan-1)
endif()
target_link_options(FPSLauncher PRIVATE -static -static-libgcc -static-libstdc++)

# If building x64, embed the x86 helper binary into a header so the x64 launcher
# can extract and run it at runtime (single EXE + two DLLs distribution).
if(ARCH STREQUAL "x64")
    set(EMBED_SRC ${CMAKE_SOURCE_DIR}/build_x86/FPSInjector.exe)
    set(EMBED_HDR ${CMAKE_SOURCE_DIR}/src/embedded_injector.h)
    add_custom_command(OUTPUT ${EMBED_HDR}
        COMMAND xxd -i ${EMBED_SRC} > ${EMBED_HDR}
        DEPENDS ${EMBED_SRC}
        COMMENT "Embedding x86 helper into header ${EMBED_HDR}"
    )
    # Ensure header is generated before compiling the launcher by listing it as a source
    target_sources(FPSLauncher PRIVATE ${EMBED_HDR})
endif()
# Small helper injector used by x64 launcher to inject into x86 games.
add_executable(FPSInjector src/helper_inject.cpp)
target_link_libraries(FPSInjector PRIVATE kernel32)
target_link_options(FPSInjector PRIVATE -static -static-libgcc -static-libstdc++)

# ----------------------------------------------------
# B. DLL (FPSLimiter.dll)
# ----------------------------------------------------
set(FPSLIMITER_SOURCES
    src/dllmain.cpp
    src/hook_d3d9.cpp
    src/hook_dxgi.cpp
    src/backend_d3d11.cpp
    src/backend_d3d12.cpp
    src/hook_opengl.cpp
)

if(ARCH STREQUAL "x64")
    list(APPEND FPSLIMITER_SOURCES src/hook_vulkan.cpp)
else()
    list(APPEND FPSLIMITER_SOURCES src/hook_vulkan_stub.cpp)
endif()

list(APPEND FPSLIMITER_SOURCES ${IMGUI_SOURCES})

add_library(FPSLimiter SHARED ${FPSLIMITER_SOURCES})
set_target_properties(FPSLimiter PROPERTIES PREFIX "") 

target_include_directories(FPSLimiter PRIVATE 
    ${minhook_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(FPSLimiter PRIVATE minhook d3d11 dxgi d3dcompiler dwmapi d3d9 d3d12 opengl32 winmm)
# Vulkan library linking depends on environment (usually vulkan-1.lib). Assuming standard name.
# Note: Since we are hooking dynamically, we might not strictly need the lib if we load pointers manually.
# But ImGui Impl Vulkan needs it.
if(ARCH STREQUAL "x64")
    target_link_libraries(FPSLimiter PRIVATE vulkan-1)
endif()

target_link_options(FPSLimiter PRIVATE -static -static-libgcc -static-libstdc++)
