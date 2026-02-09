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
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()

project(FPSLimiter)

set(CMAKE_CXX_STANDARD 17)

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
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

# ----------------------------------------------------
# A. Launcher (FPSLauncher.exe)
# ----------------------------------------------------
add_executable(FPSLauncher src/launcher.cpp ${IMGUI_SOURCES})
target_include_directories(FPSLauncher PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(FPSLauncher PRIVATE comdlg32 shlwapi d3d11 dxgi d3dcompiler dwmapi)
target_link_options(FPSLauncher PRIVATE -static -static-libgcc -static-libstdc++)

# ----------------------------------------------------
# B. DLL (FPSLimiter.dll)
# ----------------------------------------------------
add_library(FPSLimiter SHARED 
    src/dllmain.cpp 
    src/hook_d3d9.cpp
    src/hook_dxgi.cpp
    src/backend_d3d11.cpp
    src/backend_d3d12.cpp
    src/hook_opengl.cpp
    ${IMGUI_SOURCES}
)
set_target_properties(FPSLimiter PROPERTIES PREFIX "") 

target_include_directories(FPSLimiter PRIVATE 
    ${minhook_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(FPSLimiter PRIVATE minhook d3d11 dxgi d3dcompiler dwmapi d3d9 d3d12 opengl32)
target_link_options(FPSLimiter PRIVATE -static -static-libgcc -static-libstdc++)
