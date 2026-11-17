cmake_minimum_required(VERSION 3.22)
project(Vela VERSION 0.2.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ------------------------------------------------------------------------------
# Dependencies (Fetched automatically at configure time)
# ------------------------------------------------------------------------------

# 1. FreeType (official GitHub mirror for fast & reliable Windows 11 font rendering)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-13-3
    GIT_SHALLOW TRUE
)

# 2. Dear ImGui (GPU-accelerated UI)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
)

# 3. Lua 5.4.7 (Fetched via Git so MinGW CMake curl SSL issue is bypassed)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.7
    GIT_SHALLOW TRUE
)

# 4. sol2 (header-only C++ <-> Lua binding)
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG v3.5.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR include
)

FetchContent_MakeAvailable(freetype imgui lua sol2)

# Build Lua static library
file(GLOB LUA_CORE_SRCS CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/*.c")
list(REMOVE_ITEM LUA_CORE_SRCS
    "${lua_SOURCE_DIR}/lua.c"
    "${lua_SOURCE_DIR}/luac.c"
    "${lua_SOURCE_DIR}/onelua.c"
    "${lua_SOURCE_DIR}/ltests.c"
)
add_library(lua_static STATIC ${LUA_CORE_SRCS})
target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}")

# Header-only sol2 target
add_library(sol2_headers INTERFACE)
target_include_directories(sol2_headers SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
target_link_libraries(sol2_headers INTERFACE lua_static)

# Build Dear ImGui + Win32 + DX11 + FreeType static library
add_library(imgui_vela STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)
target_include_directories(imgui_vela PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    "${imgui_SOURCE_DIR}/misc/freetype"
)
target_compile_definitions(imgui_vela PUBLIC
    IMGUI_ENABLE_FREETYPE
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(imgui_vela PUBLIC freetype d3d11 dxgi d3dcompiler)

# ------------------------------------------------------------------------------
# Vela Executable
# ------------------------------------------------------------------------------
add_executable(vela WIN32
    src/main.cpp
    src/core/FileWatcher.cpp
    src/core/LuaEngine.cpp
    src/fs/FileOps.cpp
    src/fs/FileScanner.cpp
    src/fs/SystemPlaces.cpp
    src/platform/OleDragDrop.cpp
    src/platform/ThumbnailCache.cpp
    src/platform/Win32Window.cpp
    src/ui/VelaApp.cpp
    src/ui/VelaDialogs.cpp
    assets/vela.rc
)

target_include_directories(vela PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(vela PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A00000B
    SOL_ALL_SAFETIES_ON=1
)

if(MSVC)
    target_compile_options(vela PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(vela PRIVATE -Wall -Wextra)
endif()

if(MINGW)
    target_link_options(vela PRIVATE -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(vela PRIVATE
    imgui_vela
    sol2_headers
    d3d11
    dxgi
    dcomp
    dwmapi
    shell32
    ole32
    shlwapi
    uxtheme
    advapi32
    propsys
)

# Copy default vela.lua next to the built executable
add_custom_command(TARGET vela POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vela>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/vela.lua"
        "$<TARGET_FILE_DIR:vela>/config/vela.lua"
    COMMENT "Copying default config/vela.lua to output directory"
)