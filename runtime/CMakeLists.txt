cmake_minimum_required(VERSION 3.24)
project(evr_runtime CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
  SOURCE_SUBDIR build/cmake DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(zstd googletest nlohmann_json zlib)

# Windows 7 APIs (GetTickCount64, ...); TDM-GCC defaults to an older target.
add_compile_definitions(_WIN32_WINNT=0x0601)

if(MSVC)
  # The friends code marks printf-style functions with GNU attributes; MSVC ignores them.
  add_compile_definitions("__attribute__(x)=" _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/utf-8 /W3 /permissive-)
endif()

# Pure logic shared by PC and Quest: canvas, level and package builders.
add_library(evr_friends_logic INTERFACE)
target_include_directories(evr_friends_logic INTERFACE include src ${zstd_SOURCE_DIR}/lib)
target_link_libraries(evr_friends_logic INTERFACE libzstd_static)

enable_testing()
add_executable(evr_tests
  tests/test_symbol.cpp
  tests/friends/test_ui_canvas.cpp
  tests/friends/test_friends_canvas.cpp
  tests/friends/test_friends_level.cpp
  tests/friends/test_friends_package.cpp
  tests/cdn/test_cdn_index.cpp
  tests/updates/test_updates.cpp
  tests/earlyquit/test_lockout_rules.cpp
  tests/menu/test_wording.cpp
  tests/party/test_party_rules.cpp)
target_link_libraries(evr_tests PRIVATE evr_friends_logic GTest::gtest_main nlohmann_json::nlohmann_json)
add_test(NAME evr_tests COMMAND evr_tests)

# ---- PC runtime DLL ----
FetchContent_Declare(minhook URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(minhook)

# MinHook 1.3.3 ships no CMake project: build its four sources here (BSD-2-Clause).
add_library(evr_minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(evr_minhook PUBLIC ${minhook_SOURCE_DIR}/include)

# Monocypher 4 (CC0 / BSD-2-Clause): Ed25519 checks of the AtlasCDN index.
FetchContent_Declare(monocypher URL https://monocypher.org/download/monocypher-4.0.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(monocypher)
add_library(evr_monocypher STATIC ${monocypher_SOURCE_DIR}/src/monocypher.c
  ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c)
target_include_directories(evr_monocypher PUBLIC ${monocypher_SOURCE_DIR}/src ${monocypher_SOURCE_DIR}/src/optional)

# The AtlasCDN index keys this build trusts (keys/cdn_pinned.txt), compiled in as kCdnPinnedText.
set(EVR_CDN_KEYS ${CMAKE_CURRENT_SOURCE_DIR}/../keys/cdn_pinned.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EVR_CDN_KEYS})
file(READ ${EVR_CDN_KEYS} EVR_CDN_PINNED)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/cdn_pinned_keys.h
  "#pragma once\ninline constexpr const char* kCdnPinnedText = R\"KEYS(${EVR_CDN_PINNED})KEYS\";\n")

add_library(evr_core STATIC src/core/core_pc.cpp src/core/resources_pc.cpp src/core/ui_text_pc.cpp src/core/frame_pc.cpp
  src/core/events_pc.cpp src/core/actors_pc.cpp src/core/crashlog_pc.cpp)
target_include_directories(evr_core PUBLIC include)
target_link_libraries(evr_core PUBLIC evr_minhook winhttp)

add_library(evr_runtime SHARED src/runtime_main.cpp src/menu/create_join_pc.cpp src/menu/invite_pc.cpp
  src/friends/friends_install_pc.cpp src/friends/friends_panel_resources.cpp src/friends/friends_panel_pc.cpp
  src/friends/menu_friends_pack.cpp src/friends/menu_friends_profile.cpp src/friends/menu_friends_avatars.cpp
  src/social/social_pc.cpp src/login/login_pc.cpp
  src/login/unlocks_probe_pc.cpp src/cdn/cdn_pc.cpp src/updates/updates_pc.cpp
  src/earlyquit/lockout_pc.cpp src/menu/wording_pc.cpp src/menu/queue_pc.cpp)
# Parties have never run in the game: off by default, so the normal build is the one safe to install.
option(EVR_PARTY "Build the party code (src/party) into the runtime DLL" OFF)
if(EVR_PARTY)
  target_sources(evr_runtime PRIVATE src/party/party_pc.cpp)
  target_compile_definitions(evr_runtime PRIVATE EVR_PARTY=1)
endif()
set_target_properties(evr_runtime PROPERTIES OUTPUT_NAME "evr-runtime" PREFIX "")
target_link_libraries(evr_runtime PRIVATE evr_core evr_friends_logic nlohmann_json::nlohmann_json zlibstatic
  evr_monocypher bcrypt)
target_include_directories(evr_runtime PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# zlib's CMake puts zconf.h in its build tree.
target_include_directories(evr_runtime PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
if(MINGW)
  # Ship a self-contained DLL: no libstdc++/libgcc/winpthread DLLs next to the game.
  target_link_options(evr_runtime PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(evr_tests PRIVATE -static)
endif()

# The evr_runtime login declaration's build and commit (git describe of this tree; "dev" outside git).
execute_process(COMMAND git describe --always --dirty --tags WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE EVR_GIT_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE EVR_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(EVR_GIT_BUILD)
  target_compile_definitions(evr_runtime PRIVATE EVR_BUILD="${EVR_GIT_BUILD}" EVR_COMMIT="${EVR_GIT_COMMIT}")
endif()
