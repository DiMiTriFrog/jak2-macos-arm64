# Top Level CMakeLists.txt
cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(jak)
include(CTest)

if(APPLE)
    enable_language(ASM)
endif()

# Execution and graphics backends are selected at configure time.  Keeping the
# selection in one generated header prevents platform checks from being spread
# across the runtime and makes an accidental JIT fallback visible in the build.
set(OPENGOAL_EXECUTION_MODE "DYNAMIC" CACHE STRING
    "GOAL execution backend (DYNAMIC or AOT)")
set_property(CACHE OPENGOAL_EXECUTION_MODE PROPERTY STRINGS DYNAMIC AOT)
set(OPENGOAL_GRAPHICS_BACKEND "OPENGL" CACHE STRING
    "Graphics backend (OPENGL or METAL)")
set_property(CACHE OPENGOAL_GRAPHICS_BACKEND PROPERTY STRINGS OPENGL METAL)

if(NOT OPENGOAL_EXECUTION_MODE STREQUAL "DYNAMIC" AND
   NOT OPENGOAL_EXECUTION_MODE STREQUAL "AOT")
    message(FATAL_ERROR
        "OPENGOAL_EXECUTION_MODE must be DYNAMIC or AOT, got '${OPENGOAL_EXECUTION_MODE}'")
endif()

if(NOT OPENGOAL_GRAPHICS_BACKEND STREQUAL "OPENGL" AND
   NOT OPENGOAL_GRAPHICS_BACKEND STREQUAL "METAL")
    message(FATAL_ERROR
        "OPENGOAL_GRAPHICS_BACKEND must be OPENGL or METAL, got '${OPENGOAL_GRAPHICS_BACKEND}'")
endif()

set(OG_TARGET_IOS 0)
set(OG_TARGET_MACOS 0)
if(CMAKE_SYSTEM_NAME MATCHES "iOS|tvOS|watchOS")
    set(OG_TARGET_IOS 1)
elseif(APPLE)
    set(OG_TARGET_MACOS 1)
endif()

if(OG_TARGET_IOS AND NOT OPENGOAL_EXECUTION_MODE STREQUAL "AOT")
    message(FATAL_ERROR "iOS targets require OPENGOAL_EXECUTION_MODE=AOT")
endif()

if(OG_TARGET_IOS AND NOT OPENGOAL_GRAPHICS_BACKEND STREQUAL "METAL")
    message(FATAL_ERROR "iOS targets require OPENGOAL_GRAPHICS_BACKEND=METAL")
endif()

option(OG_BUILD_HOST_TOOLS "Build desktop-only compiler and authoring tools" ON)
if(OG_TARGET_IOS AND OG_BUILD_HOST_TOOLS)
    message(STATUS "Disabling desktop-only host tools for iOS target")
    set(OG_BUILD_HOST_TOOLS OFF CACHE BOOL "Build desktop-only compiler and authoring tools" FORCE)
endif()

set(OPENGOAL_AOT_GENERATED_DIR "" CACHE PATH
    "Read-only directory containing generated Jak II AOT input")
set(OPENGOAL_AOT_GENERATION_TRIPLE "arm64-apple-macos"
    CACHE STRING "Target triple recorded by the host AOT generator")

if(OPENGOAL_EXECUTION_MODE STREQUAL "AOT" AND
   NOT OPENGOAL_AOT_GENERATED_DIR STREQUAL "")
    get_filename_component(OG_AOT_INPUT_DIR "${OPENGOAL_AOT_GENERATED_DIR}" ABSOLUTE)
    if(NOT IS_DIRECTORY "${OG_AOT_INPUT_DIR}")
        message(FATAL_ERROR
            "OPENGOAL_AOT_GENERATED_DIR must be an existing generated-input directory: "
            "${OG_AOT_INPUT_DIR}")
    endif()
    foreach(OG_AOT_REQUIRED_FILE manifest.json build-report.json)
        if(NOT EXISTS "${OG_AOT_INPUT_DIR}/${OG_AOT_REQUIRED_FILE}")
            message(FATAL_ERROR
                "AOT generated input is missing ${OG_AOT_REQUIRED_FILE} in ${OG_AOT_INPUT_DIR}")
        endif()
    endforeach()
    set(OPENGOAL_AOT_GENERATED_DIR "${OG_AOT_INPUT_DIR}" CACHE PATH
        "Read-only directory containing generated Jak II AOT input" FORCE)
endif()

if(OPENGOAL_EXECUTION_MODE STREQUAL "AOT")
    set(OG_EXECUTION_MODE_AOT 1)
    set(OG_EXECUTION_MODE_DYNAMIC 0)
else()
    set(OG_EXECUTION_MODE_AOT 0)
    set(OG_EXECUTION_MODE_DYNAMIC 1)
endif()

if(OPENGOAL_GRAPHICS_BACKEND STREQUAL "METAL")
    set(OG_GRAPHICS_BACKEND_METAL 1)
    set(OG_GRAPHICS_BACKEND_OPENGL 0)
else()
    set(OG_GRAPHICS_BACKEND_METAL 0)
    set(OG_GRAPHICS_BACKEND_OPENGL 1)
endif()

set(OG_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${OG_GENERATED_INCLUDE_DIR}/common/platform")
configure_file(
    "${PROJECT_SOURCE_DIR}/common/platform/BuildConfig.h.in"
    "${OG_GENERATED_INCLUDE_DIR}/common/platform/BuildConfig.h"
    @ONLY)
include_directories("${OG_GENERATED_INCLUDE_DIR}")

# Include third-party modules
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/third-party/cmake/modules/)

# Default to Release mode
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# Potentially statically build the project
option(STATICALLY_LINK "Build for release purposes (statically link everything)" OFF)
message(STATUS "Statically Link? ${STATICALLY_LINK}")
if(STATICALLY_LINK)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
else()
    set(BUILD_SHARED_LIBS ON)
    set(BUILD_STATIC_LIBS OFF)
endif()

# For clangd
# file(COPY), not configure_file(): configure_file registers its input as a
# configure dependency, putting the ever-rewritten compile_commands.json into
# the Ninja regen rule and causing an endless reconfigure loop.
if (EXISTS "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json" )
    file(COPY
        "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
        DESTINATION "${PROJECT_SOURCE_DIR}/build")
endif()

# NOTE - for SDL, i think SDL3 regressed this - https://github.com/libsdl-org/SDL/pull/6455
# a more recent issue - https://github.com/libsdl-org/SDL/issues/12078
if (APPLE)
  enable_language(OBJC)
  if(OPENGOAL_GRAPHICS_BACKEND STREQUAL "METAL")
    enable_language(OBJCXX)
  endif()
  if(NOT OG_TARGET_IOS)
    execute_process(
      COMMAND xcrun --show-sdk-path
          OUTPUT_VARIABLE MACOSX_SYSROOT
          OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      # Tell CMake to use it only for a macOS build. An iOS preset owns its
      # iphoneos/iphonesimulator SDK selection.
      set(CMAKE_OSX_SYSROOT "${MACOSX_SYSROOT}" CACHE PATH "macOS SDK path" FORCE)
  endif()
endif()

# Setup compiler flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "Clang Detected - Setting Defaults")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} \
        -fcxx-exceptions \
        -fexceptions \
        -fdiagnostics-color=always \
        -mavx \
        -Wall \
        -Wno-c++11-narrowing \
        -Wno-c++98-compat \
        -Wno-deprecated-this-capture \
        -Wno-missing-field-initializers \
        -Wno-sign-compare \
        -Wno-ignored-qualifiers \
        -O3 \
        -D_CRT_SECURE_NO_WARNINGS")

    # Increase stack size for windows, who's default is too low
    if(WIN32)
        # Increase the reserved stack size for all threads to 16MB
        # Note: this is only _reserved_ memory, not necessarily _committed_ memory
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LDFLAGS} -Xlinker /STACK:16000000")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -g")
    endif()

    # additional c++ and linker flags for release mode for our projects
    if(CMAKE_BUILD_TYPE MATCHES "Release" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    endif()

    if(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LDFLAGS} -Xlinker /debug")
    endif()

    # Fuzzing
    if(NOT STATICALLY_LINK AND ASAN_BUILD)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -O1")
        message(STATUS "Doing ASAN build")
    endif()

    # Warnings
    set(THIRDPARTY_IGNORED_WARNINGS "-Wno-everything")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "GCC detected - Setting Defaults")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} \
        -Wall \
        -Winit-self \
        -ggdb \
        -Wextra \
        -Wno-cast-align \
        -Wcast-qual \
        -Wdisabled-optimization \
        -Wformat \
        -Wextra \
        -Woverloaded-virtual \
        -Wredundant-decls \
        -Wshadow \
        -Wsign-promo \
        -fdiagnostics-color=always \
        -mavx")

    # additional c++ flags for release mode for our projects
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    endif()
    set(THIRDPARTY_IGNORED_WARNINGS "-w")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(STATUS "AppleClang detected - Setting Defaults")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} \
        -Wall \
        -Winit-self \
        -ggdb \
        -Wextra \
        -Wno-cast-align \
        -Wcast-qual \
        -Wdisabled-optimization \
        -Wformat \
        -Wextra \
        -Woverloaded-virtual \
        -Wredundant-decls \
        -Wshadow \
        -Wsign-promo \
        -fdiagnostics-color=always"
        )

    # Host-specific tuning is not valid for iOS and must not leak into a
    # device/simulator build. macOS keeps the existing architecture tuning.
    if(NOT OG_TARGET_IOS)
        if (CMAKE_APPLE_SILICON_PROCESSOR STREQUAL "x86_64")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx")
        else()
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -mcrc")
        endif()
    endif()

    # additional c++ flags for release mode for our projects
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    endif()
    set(THIRDPARTY_IGNORED_WARNINGS "-w")
    if(NOT OG_TARGET_IOS)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-stack_size -Wl,0x20000000")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(STATUS "MSVC detected - Setting Defaults")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /utf-8 /arch:AVX")

    # Increase stack size for windows, who's default is too low
    # Increase the reserved stack size for all threads to 16MB
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:16000000,16384")

    # additional c++ and linker flags for specific build types
    if(CMAKE_BUILD_TYPE MATCHES "Release" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O2 /Ob2")
    endif()

    if(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DEBUG")
    endif()
    set(THIRDPARTY_IGNORED_WARNINGS "/w")
else()
    message(FATAL_ERROR "Unknown Compiler '${CMAKE_CXX_COMPILER_ID}', get out!")
endif()

# Platform Specific Settings
if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Code Coverage
option(CODE_COVERAGE "Enable Code Coverage Compiler Flags" OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CODE_COVERAGE)
    include(CodeCoverage)
    append_coverage_compiler_flags()
    message(STATUS "Code Coverage build is enabled!")
endif()

function(build_third_party_lib dir_name target_name)
    add_subdirectory(third-party/${dir_name} EXCLUDE_FROM_ALL)
    # Convert all public include dirs to SYSTEM
    get_target_property(INCLUDES ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
    if(INCLUDES)
        set_target_properties(${target_name} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${INCLUDES}"
        )
    endif()
endfunction()

# Dependencies and Libraries
# includes relative to top level jak-project folder
include_directories(./)

# build templating engine library
include_directories(SYSTEM third-party/inja)

# libcurl for HTTP requests
# Enable SSL Support, most URLs now-a-days use SSL!
# TODO - probably integrate with ZSTD since we have it already
set(CURL_USE_LIBSSH2 OFF)
# another change in the macOS test runner environment that suddenly has broken
# but this is another part of libcurl we probably don't need (multi-threading/parallelism)
set(CURL_USE_LIBPSL OFF)
set(CURL_ZLIB OFF)
# these default to AUTO since curl 8.10 and would silently pick up system libs
set(CURL_BROTLI OFF)
set(CURL_ZSTD OFF)
# curl's BUILD_EXAMPLES cache option (default ON) would otherwise leak into
# discord-rpc's identically named option and break its examples add_subdirectory
set(BUILD_EXAMPLES OFF)
# suddenly became enabled on macOS atleast, only needed for unicode URLs
# wasn't being properly statically linked
set(USE_LIBIDN2 OFF)
if(WIN32)
    set(CURL_USE_SCHANNEL ON) # native Windows SSL support
elseif(OG_TARGET_IOS)
    # The iOS runtime has no host-side updater/download path. Do not search
    # for Homebrew/OpenSSL while cross-compiling; platform data is packaged
    # by the iOS data pipeline instead.
    set(CURL_USE_OPENSSL OFF)
    set(CURL_USE_SECTRANSP OFF)
else()
    # curl 8.15 removed the native macOS backend (CURL_USE_SECTRANSP), so macOS
    # uses OpenSSL like linux (Homebrew openssl@3, see the macOS build workflows)
    if(STATICALLY_LINK)
        set(OPENSSL_USE_STATIC_LIBS TRUE)
    endif()
    set(CURL_USE_OPENSSL ON)
endif()
include_directories(third-party/curl/include)
if(STATICALLY_LINK)
    build_third_party_lib(curl libcurl_static)
else()
    build_third_party_lib(curl libcurl_shared)
endif()

# build repl library
build_third_party_lib(replxx replxx)

# SQLite - Jak 2/3's built in editor
add_definitions(-DHAVE_USLEEP=1)
add_definitions(-DSQLITE_THREADSAFE=1)
add_definitions(-DSQLITE_ENABLE_JSON1)
add_definitions(-DSQLITE_ENABLE_RTREE)
build_third_party_lib(sqlite3 sqlite3)

# build tree-sitter parser
include_directories(third-party/tree-sitter/tree-sitter/lib/include)
build_third_party_lib(tree-sitter tree-sitter)

# native OS dialogs for error messages
build_third_party_lib(libtinyfiledialogs libtinyfiledialogs)

# build format library
include_directories(third-party/fmt/include)
build_third_party_lib(fmt fmt)

# build common library
add_subdirectory(common)

# build decompiler
if(OG_BUILD_HOST_TOOLS)
    add_subdirectory(decompiler)
endif()

# cubeb - desktop audio. iOS uses its platform audio backend and must not
# compile the macOS CoreAudio device implementation.
if(NOT OG_TARGET_IOS)
    build_third_party_lib(cubeb cubeb)
endif()

# build LSP
if(OG_BUILD_HOST_TOOLS)
    add_subdirectory(lsp)
endif()

# build libco and zstd
build_third_party_lib(libco libco)
build_third_party_lib(zstd libzstd_static)

# build SDL
include(SDLOptions)
if(STATICALLY_LINK)
    build_third_party_lib(SDL SDL3-static)
else()
    build_third_party_lib(SDL SDL3-shared)
endif()

# build imgui
include_directories(third-party/glad/include)
include_directories(third-party/SDL/include)
build_third_party_lib(imgui imgui)

# build the game code in C++
add_subdirectory(game)

# build the compiler
if(OG_BUILD_HOST_TOOLS)
    add_subdirectory(goalc)
endif()

# AOT is deliberately a two-stage build.  The generator is a host executable
# and is never part of an iOS target.  The runtime consumes an explicitly
# supplied directory; it must never invoke a host tool as a side effect of a
# signed/cross-compiled build.
if(OG_BUILD_HOST_TOOLS)
    add_custom_target(aot-host-tools
        DEPENDS goalc jak2-aot-generator)

    set(OG_AOT_GENERATOR_OUTPUT_DIR
        "${OPENGOAL_AOT_GENERATED_DIR}")
    if(OG_AOT_GENERATOR_OUTPUT_DIR STREQUAL "")
        set(OG_AOT_GENERATOR_OUTPUT_DIR
            "${CMAKE_BINARY_DIR}/aot-generated/jak2")
    endif()

    add_custom_target(generate-jak2-aot
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OG_AOT_GENERATOR_OUTPUT_DIR}"
        COMMAND $<TARGET_FILE:jak2-aot-generator>
                --source-root "${PROJECT_SOURCE_DIR}"
                --output-dir "${OG_AOT_GENERATOR_OUTPUT_DIR}"
                --target-triple "${OPENGOAL_AOT_GENERATION_TRIPLE}"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        DEPENDS jak2-aot-generator
        VERBATIM)

    add_custom_target(verify-jak2-aot-determinism
        COMMAND ${CMAKE_COMMAND} -E env
                AOT_GENERATOR="$<TARGET_FILE:jak2-aot-generator>"
                AOT_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
                AOT_TARGET_TRIPLE="${OPENGOAL_AOT_GENERATION_TRIPLE}"
                python3 "${PROJECT_SOURCE_DIR}/scripts/verify_aot_determinism.py"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        DEPENDS jak2-aot-generator
        VERBATIM)

    add_custom_target(verify-jak2-aot-completeness
        COMMAND python3 "${PROJECT_SOURCE_DIR}/scripts/verify_aot_completeness.py"
                "${OG_AOT_GENERATOR_OUTPUT_DIR}/build-report.json"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        DEPENDS generate-jak2-aot
        VERBATIM)
endif()

# build standalone tools
if(OG_BUILD_HOST_TOOLS)
    add_subdirectory(tools)
endif()

# build the gtest libraries
if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

build_third_party_lib(googletest gtest)
include(GoogleTest)

# build tests
if(OG_BUILD_HOST_TOOLS)
    include(test/CMakeLists.txt)
endif()

# build lzokay library
build_third_party_lib(lzokay lzokay)

build_third_party_lib(stb_image stb_image)
# build draco library for tinygltf compression support
include_directories(third-party/draco/src)
add_subdirectory(third-party/draco)
add_compile_definitions(TINYGLTF_ENABLE_DRACO)
build_third_party_lib(tiny_gltf tiny_gltf)
build_third_party_lib(xdelta3 xdelta3)

# Discord rich presence is a desktop integration. iOS has no Discord RPC
# dependency in the runtime target.
if(NOT OG_TARGET_IOS)
    include_directories(third-party/discord-rpc/include)
    build_third_party_lib(discord-rpc discord-rpc)
endif()

# build zydis third party library for disassembling x86
# TODO - Once under CMake 3.13's policy CMP0077, override with `set()` instead
option(ZYDIS_BUILD_TOOLS "Zydis: Build tools" OFF)
option(ZYDIS_BUILD_EXAMPLES "Zydis: Build examples" OFF)

if(STATICALLY_LINK)
    message(STATUS "Statically linking zydis")
    option(ZYDIS_BUILD_SHARED_LIB "Zydis: Build shared library" OFF)
else()
    message(STATUS "dynamically linking zydis")
    option(ZYDIS_BUILD_SHARED_LIB "Zydis: Build shared library" ON)
endif()

build_third_party_lib(zydis Zydis)

# windows memory management lib
if(WIN32)
    build_third_party_lib(mman mman)
endif()
