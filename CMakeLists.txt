cmake_minimum_required(VERSION 3.15)

# Default toolchain — applied only when the user did not pass these on the
# command line or via env (CC/CXX). Must run BEFORE project() so the project()
# call's compiler detection sees our defaults.
if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER clang++)
endif()

# Default build type — RelWithDebInfo unless the user explicitly chose one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

project(qubic CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force Clang >= 18
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18.0)
        message(FATAL_ERROR "Clang version 18 or higher is required. Found: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # Note: AppleClang has different versioning.
    # AppleClang 15 is roughly equivalent to LLVM Clang 16/17.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15.0)
        message(FATAL_ERROR "AppleClang 15+ required for this project.")
    endif()
else()
    message(WARNING "You are not using Clang. This project is optimized for Clang 18+.")
endif()

# Load project CMake modules.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerSetup)
include(HomebrewDependencies)

if(IS_WINDOWS)
    add_compile_definitions(
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
endif()

# Make keg-only dependencies visible to ordinary find_package calls.
qubic_add_homebrew_prefixes(
    zlib
    openssl@3
    libffi
)

# Build options
option(BUILD_TESTS "Build the test suite" OFF)
option(BUILD_BENCHMARK "Build the EFI benchmark application" OFF)
option(BUILD_BINARY "Build the EFI application" ON)
option(USE_SANITIZER "Build test with sanitizer support (clang only)" ON)
option(LITE_WASM_SC "Enable testnet-lite-RAM Wasm smart contracts" OFF)
option(ANT_WALKER "Build the ant-colony walker sidecar and its node client" ON)
set(QUBIC_STANDALONE_RELEASE ON CACHE INTERNAL "Standalone builds are the only supported configuration" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Use static OpenSSL" FORCE)
set(ZLIB_USE_STATIC_LIBS TRUE CACHE BOOL "Use static zlib" FORCE)
if(NOT IS_WINDOWS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

if(LITE_WASM_SC AND (NOT TESTNET OR NOT TESTNET_LITE_RAM))
    message(FATAL_ERROR "LITE_WASM_SC requires TESTNET=ON and TESTNET_LITE_RAM=ON")
endif()

if(IS_WINDOWS)
    message(WARNING "Building on Windows using Visual Studio and CMake has undergone limited testing and is not officially supported at this time.")
    if(USE_SANITIZER)
        message(WARNING "Building tests with sanitizer support is not supported on Windows.")
    endif()
endif()

# Blosc2 is needed by both src/ and test/ (via src/extensions/zipper.h pulled in by
# src/platform/virtual_memory.h). Declare it here so both subdirectories see the
# target regardless of which is added first.
include(FetchContent)
set(BUILD_SHARED OFF CACHE BOOL "Disable Blosc2 shared library" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable Blosc2 examples" FORCE)
set(BUILD_TESTS_BLOSC OFF CACHE BOOL "Disable Blosc2 tests" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "Disable Blosc2 benchmarks" FORCE)
set(PREFER_EXTERNAL_ZLIB ON CACHE BOOL "Use the existing zlib dependency in Blosc2" FORCE)
FetchContent_Declare(
        blosc2
        GIT_REPOSITORY https://github.com/Blosc/c-blosc2.git
        GIT_TAG        v2.15.1
)
FetchContent_MakeAvailable(blosc2)
include_directories(${blosc2_SOURCE_DIR}/include)

# blosc2 enables NASM, but the Windows .asm sources use MASM.
if(WIN32)
    list(REMOVE_ITEM CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS asm)
endif()

# Keep the WAMR runtime SHA-pinned because it is consensus-critical.
# The nonexistent source directory makes FetchContent download-only.
if(LITE_WASM_SC)
    FetchContent_Declare(
        wamr
        GIT_REPOSITORY https://github.com/bytecodealliance/wasm-micro-runtime.git
        GIT_TAG        0e47872fb4e57833dbeccf7c312a406fa2dac2bc
        SOURCE_SUBDIR  does-not-exist
    )
    FetchContent_MakeAvailable(wamr)
    set(WAMR_ROOT_DIR ${wamr_SOURCE_DIR})
endif()

# Always include platform_common as it's needed for both application and tests
add_subdirectory(lib/platform_common)

if(BUILD_BINARY OR BUILD_BENCHMARK)
    add_subdirectory(lib/platform_efi)
endif()

# Build the tests first. On fail, the build will fail completely
if(BUILD_TESTS)
    message(STATUS "--- Test suite ---")
    enable_testing()
    add_subdirectory(lib/platform_os)

    # If we're not building the application, we still need src for tests
    # but don't make it a default target
    if(NOT BUILD_BINARY)
        add_subdirectory(src EXCLUDE_FROM_ALL)
    endif()

    add_subdirectory(test)
endif()

# Build the application if requested
if(BUILD_BINARY)
    message(STATUS "--- EFI Core application ---")
    add_subdirectory(src)
endif()

# Add the UEFI m256i benchmark
if(BUILD_BENCHMARK)
    message(STATUS "-- EFI Benchmark ---")
    add_subdirectory(benchmark_uefi)
endif()

# Reject bare std::mutex declarations that escape the fork lock census on Unix builds.
if(UNIX)
    add_custom_target(check_smart_mutex ALL
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_smart_mutex.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "check_smart_mutex: scanning for unsanctioned std::mutex declarations (fork census)")
    if(TARGET Qubic)
        add_dependencies(Qubic check_smart_mutex)
    endif()
    if(TARGET qubic_core_tests)
        add_dependencies(qubic_core_tests check_smart_mutex)
    endif()
endif()
