cmake_minimum_required(VERSION 3.18)

# Must be set before project(): MLBB loads this library through its own
# dlopen into a context where libc++_shared.so is not resolvable, so libc++ has
# to be linked statically. Every lib the game loads from app_libs links only
# liblog/libm/libdl/libc/libandroid.
set(ANDROID_STL c++_static CACHE STRING "" FORCE)

project(rain CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Keep the output lean: game processes are sensitive to lib size, and a stripped
# BIND_NOW .so matches how the reference implementation is built.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--gc-sections -Wl,-z,now -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL -static-libstdc++")

add_compile_definitions(
    GL_GLEXT_PROTOTYPES
    IMGUI_IMPL_OPENGL_ES3
)

# ---------------- probe target ----------------
# Tiny standalone .so: does MLBB's loader load arbitrary libs dropped into app_libs?
add_library(raintest SHARED probe/raintest.cpp)
target_link_libraries(raintest log)
set_target_properties(raintest PROPERTIES OUTPUT_NAME "raintest")

# ---------------- rain ----------------
file(GLOB IMGUI_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui/imgui.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui/imgui_draw.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui/imgui_tables.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui/imgui_widgets.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui/backends/imgui_impl_opengl3.cpp"
)

add_library(rain SHARED
    ${IMGUI_SRC}
    src/rain.cpp
    src/il2cpp.cpp
    src/game.cpp
    src/input.cpp
    src/overlay.cpp
    src/esp.cpp
    src/maphack.cpp
    src/menu.cpp
    src/synth.cpp
    src/persist.cpp
    src/cfgjson.cpp
    src/account.cpp
    src/notify.cpp
    src/icons.cpp
    src/autox.cpp
    src/target.cpp
)

target_include_directories(rain PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/GlossHook/GlossHook/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# GlossHook ships static libs for ARM (32-bit) and ARM64 only — it has no x86
# backend (its header hard-errors on other architectures). Select the right one
# per ABI so both supported targets build from the same tree.
set(GLOSSHOOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/GlossHook/GlossHook/lib")
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(GLOSSHOOK_LIB "${GLOSSHOOK_ROOT}/ARM/libGlossHook.a")
elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    set(GLOSSHOOK_LIB "${GLOSSHOOK_ROOT}/ARM64/libGlossHook.a")
else()
    message(FATAL_ERROR
        "Rain only builds for armeabi-v7a and arm64-v8a: the GlossHook hook "
        "backend is ARM-only, and MLBB ships only arm64-v8a. Requested: ${ANDROID_ABI}")
endif()

target_link_libraries(rain
    log android EGL GLESv3
    "${GLOSSHOOK_LIB}"
    dl
)

set_target_properties(rain PROPERTIES OUTPUT_NAME "rain")

# The version script hides all non-JNI symbols — rain only; the raintest
# probe defines no JNI symbols and must link without it.
target_link_options(rain PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/rain.map")

# Release hygiene: strip .symtab/.debug_* from the shipped .so. The version
# script already hides all non-JNI symbols; this removes the remaining ballast
# (~412 KB on v26) and any implementation metadata.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET rain POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:rain>
        COMMENT "Stripping rain (release hygiene)"
        VERBATIM
    )
endif()

# ---------------- rinject ----------------
# Standalone hot-attach injector (XNISH's second load model): run as root on
# the device, finds the game process, attaches via ptrace and calls dlopen on
# rain.so inside it — no force-stop needed, so the game's config and session
# survive the attach.
add_executable(rinject injector/rinject.cpp)
target_link_libraries(rinject log)
set_target_properties(rinject PROPERTIES OUTPUT_NAME "rinject")
