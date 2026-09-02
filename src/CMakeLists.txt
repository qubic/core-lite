cmake_minimum_required(VERSION 3.15)
project(qubic_core CXX)

if(NOT WIN32)
    enable_language(ASM_NASM)
endif()

# GoogleTest requires at least C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libraries" FORCE)
if(NOT WIN32)
    # Unix builds link static archives; Windows uses its native suffixes.
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

# Enable FetchContent module
include(FetchContent)
include(ExternalProject)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/lib/)

# Make Homebrew headers and libraries visible to macOS builds.
if(IS_MACOS)
    if(EXISTS /opt/homebrew)            # Apple Silicon
        set(BREW_PREFIX /opt/homebrew)
    else()                              # Intel mac
        set(BREW_PREFIX /usr/local)
    endif()
    include_directories(SYSTEM ${BREW_PREFIX}/include ${BREW_PREFIX}/opt/openssl@3/include)
    link_directories(${BREW_PREFIX}/lib ${BREW_PREFIX}/opt/openssl@3/lib)
endif()

if(IS_ARM)
    # ARM uses system stack recovery and a shim for x86 CPUID includes.
    include_directories(${CMAKE_SOURCE_DIR}/lib/platform_common/arm_shims)
    set(CUSTOM_STACK_OBJ ${CMAKE_CURRENT_SOURCE_DIR}/platform/custom_stack_arm_stub.cpp)
elseif(WIN32 OR IS_MACOS)
    # Windows and macOS use system stack recovery instead of the Linux NASM object.
    set(CUSTOM_STACK_OBJ ${CMAKE_CURRENT_SOURCE_DIR}/platform/custom_stack_arm_stub.cpp)
else()
    set(ASM_FILE "${CMAKE_CURRENT_SOURCE_DIR}/platform/custom_stack.nasm")
    set(ASM_OBJ "${CMAKE_CURRENT_BINARY_DIR}/custom_stack.obj")
    add_custom_command(
        OUTPUT ${ASM_OBJ}
        COMMAND ${NASM_EXECUTABLE} -f elf64 ${ASM_FILE} -o ${ASM_OBJ}
        DEPENDS ${ASM_FILE}
        COMMENT "Assembling ${ASM_FILE}"
    )
    set(CUSTOM_STACK_OBJ ${ASM_OBJ})
endif()

# Extra libraries
set(JSONCPP_WITH_POST_INSTALL OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        jsoncpp
        GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
        GIT_TAG 1.9.6
)
FetchContent_MakeAvailable(jsoncpp)
set(JSONCPP_FOUND TRUE CACHE BOOL "" FORCE)
set(Jsoncpp_FOUND TRUE CACHE BOOL "" FORCE)
set(JSONCPP_INCLUDE_DIRS "${jsoncpp_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(JSONCPP_INCLUDE_DIR "${jsoncpp_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(JSONCPP_LIBRARIES jsoncpp_static CACHE STRING "" FORCE)
if(NOT TARGET Jsoncpp::Jsoncpp)
    add_library(Jsoncpp::Jsoncpp ALIAS jsoncpp_static)
endif()
include_directories(${jsoncpp_SOURCE_DIR}/include)

set(BUILD_POSTGRESQL OFF CACHE BOOL "" FORCE)
set(BUILD_MYSQL OFF CACHE BOOL "" FORCE)
set(BUILD_SQLITE OFF CACHE BOOL "" FORCE)
set(BUILD_REDIS OFF CACHE BOOL "" FORCE)
set(BUILD_DROGON_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG v1.9.11
)
FetchContent_MakeAvailable(drogon)

FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 12.1.0
)
FetchContent_MakeAvailable(fmt)

# libbacktrace is only needed by the Linux Boost stacktrace backend.
if(NOT APPLE AND NOT WIN32)
set(LIBBACKTRACE_PREFIX ${CMAKE_BINARY_DIR}/libbacktrace)
set(LIBBACKTRACE_BINARY_DIR ${LIBBACKTRACE_PREFIX}/src/libbacktrace_project-build)
set(LIBBACKTRACE_STATIC_LIB ${LIBBACKTRACE_BINARY_DIR}/.libs/libbacktrace.a)
set(LIBBACKTRACE_INCLUDE_DIR ${LIBBACKTRACE_PREFIX}/src/libbacktrace_project/)
file(MAKE_DIRECTORY ${LIBBACKTRACE_INCLUDE_DIR})
ExternalProject_Add(libbacktrace_project
        GIT_REPOSITORY https://github.com/ianlancetaylor/libbacktrace.git
        GIT_TAG master
        PREFIX ${LIBBACKTRACE_PREFIX}
        BINARY_DIR ${LIBBACKTRACE_BINARY_DIR}

        UPDATE_COMMAND ""
        LOG_CONFIGURE 1    # Hides the "checking..." lines
        LOG_BUILD 1        # Hides the "libtool: compile..." lines
        LOG_DOWNLOAD 1     # Hides the Git clone progress
        LOG_INSTALL 1      # Hides the install step output

        # Run the Autotools configuration
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --with-pic --enable-static --disable-shared
        # Build the project
        BUILD_COMMAND make -j${nproc}
        # We don't want to install it to /usr/local, keep it in our build folder
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${LIBBACKTRACE_STATIC_LIB}
)
add_library(backtrace_static STATIC IMPORTED)
set_target_properties(backtrace_static PROPERTIES
        IMPORTED_LOCATION ${LIBBACKTRACE_STATIC_LIB}
        INTERFACE_INCLUDE_DIRECTORIES ${LIBBACKTRACE_INCLUDE_DIR}
)
add_dependencies(backtrace_static libbacktrace_project)
include_directories(SYSTEM ${LIBBACKTRACE_INCLUDE_DIR})
endif() # NOT APPLE AND NOT WIN32 (libbacktrace)

# Define the shared library (matches Visual Studio DynamicLibrary configuration)
add_executable(
  Qubic
  qubic.cpp
  ${CUSTOM_STACK_OBJ}
)
set_target_properties(Qubic PROPERTIES SKIP_BUILD_RPATH TRUE)

# Keep the tracked header used by native MSBuild byte-for-byte in sync with
# the canonical task without rewriting the source tree during normal builds.
set(BPP9000_TASK_FILE ${CMAKE_SOURCE_DIR}/data/bpp9000.task)
set(BPP9000_TASK_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/mining/bpp9000_task.generated.h)
set(BPP9000_TASK_CHECK_HEADER ${CMAKE_CURRENT_BINARY_DIR}/bpp9000_task.generated.h)
add_custom_target(
    bpp9000_task_check
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${BPP9000_TASK_FILE}
            -DOUTPUT_FILE=${BPP9000_TASK_CHECK_HEADER}
            -DEXPECTED_FILE=${BPP9000_TASK_HEADER}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    DEPENDS
            ${BPP9000_TASK_FILE}
            ${BPP9000_TASK_HEADER}
            ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
    COMMENT "Checking embedded bpp9000 task"
    VERBATIM
)
add_dependencies(Qubic bpp9000_task_check)

# Explorer asset bundling: regen embedded header when HTML/CSS/JS change.
if(NOT WIN32)
    set(EXPLORER_ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extensions/http/static/explorer)
    set(EXPLORER_ASSETS_OUT ${CMAKE_CURRENT_SOURCE_DIR}/extensions/http/controller/explorer_assets.generated.h)
    add_custom_command(
        OUTPUT  ${EXPLORER_ASSETS_OUT}
        DEPENDS ${EXPLORER_ASSETS_DIR}/index.html
                ${EXPLORER_ASSETS_DIR}/style.css
                ${EXPLORER_ASSETS_DIR}/app.js
                ${EXPLORER_ASSETS_DIR}/regen.sh
        COMMAND bash ${EXPLORER_ASSETS_DIR}/regen.sh
        COMMENT "Regenerating explorer embedded assets header"
        VERBATIM
    )
    add_custom_target(explorer_assets_gen DEPENDS ${EXPLORER_ASSETS_OUT})
    add_dependencies(Qubic explorer_assets_gen)
else()
    # Generate the same explorer header without requiring bash on Windows.
    set(EXPLORER_ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extensions/http/static/explorer)
    set(EXPLORER_ASSETS_OUT ${CMAKE_CURRENT_SOURCE_DIR}/extensions/http/controller/explorer_assets.generated.h)
    add_custom_command(
        OUTPUT  ${EXPLORER_ASSETS_OUT}
        DEPENDS ${EXPLORER_ASSETS_DIR}/index.html
                ${EXPLORER_ASSETS_DIR}/style.css
                ${EXPLORER_ASSETS_DIR}/app.js
                ${EXPLORER_ASSETS_DIR}/regen.cmake
        COMMAND ${CMAKE_COMMAND} -DSRC=${EXPLORER_ASSETS_DIR} -DOUT=${EXPLORER_ASSETS_OUT} -P ${EXPLORER_ASSETS_DIR}/regen.cmake
        COMMENT "Regenerating explorer embedded assets header (cmake -P)"
        VERBATIM
    )
    add_custom_target(explorer_assets_gen DEPENDS ${EXPLORER_ASSETS_OUT})
    add_dependencies(Qubic explorer_assets_gen)
endif()

if(TESTNET)
    target_compile_definitions(Qubic PRIVATE TESTNET)
endif()

if(ONLY_LOGGING)
    target_compile_definitions(Qubic PRIVATE ONLY_LOGGING)
endif()

if(CMAKE_NO_USE_SWAP)
    target_compile_definitions(Qubic PRIVATE CMAKE_NO_USE_SWAP)
endif()

if(NO_RPC)
    target_compile_definitions(Qubic PRIVATE NO_RPC)
endif()

# Peer port override, so a second node can run where the default is taken.
if(DEFINED PORT)
    target_compile_definitions(Qubic PRIVATE PORT=${PORT})
endif()

if(NO_ENABLE_QUBIC_LOGGING_EVENT)
    target_compile_definitions(Qubic PRIVATE NO_ENABLE_QUBIC_LOGGING_EVENT)
endif()

if(TESTNET)
    target_compile_definitions(Qubic PRIVATE TESTNET)
endif()

if(TESTNET_PREFILL_QUS)
    target_compile_definitions(Qubic PRIVATE TESTNET_PREFILL_QUS)
endif()

if(TESTNET_LITE_RAM)
    target_compile_definitions(Qubic PRIVATE TESTNET_LITE_RAM)
endif()

# The root configuration enforces the Wasm testnet prerequisites.
if(LITE_WASM_SC)
    target_compile_definitions(Qubic PRIVATE LITE_WASM_SC)
    enable_language(C)

    # WAMR_ROOT_DIR is fetched and SHA-pinned by the root CMakeLists.
    if(APPLE)
        set(WAMR_BUILD_PLATFORM "darwin")
    elseif(WIN32)
        set(WAMR_BUILD_PLATFORM "windows")
    else()
        set(WAMR_BUILD_PLATFORM "linux")
    endif()
    if(IS_ARM)
        set(WAMR_BUILD_TARGET "AARCH64")
    else()
        set(WAMR_BUILD_TARGET "X86_64")
    endif()
    set(WAMR_BUILD_INTERP 1)
    set(WAMR_BUILD_FAST_INTERP 0)
    set(WAMR_BUILD_DUMP_CALL_STACK 1)
    set(WAMR_BUILD_AOT 0)
    set(WAMR_BUILD_JIT 0)
    set(WAMR_BUILD_LIBC_BUILTIN 0)
    set(WAMR_BUILD_LIBC_WASI 0)
    set(WAMR_BUILD_MULTI_MODULE 0)
    set(WAMR_BUILD_REF_TYPES 1)
    # Use software bounds checks because Wasm runs on existing node threads.
    set(WAMR_DISABLE_HW_BOUND_CHECK 1)
    # test/CMakeLists.txt builds the same WAMR runtime; whichever is configured first owns vmlib.
    if(NOT TARGET vmlib)
        include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
        add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
        if(WIN32)
            # WAMR's Windows assembly uses MASM syntax.
            foreach(_wsrc ${WAMR_RUNTIME_LIB_SOURCE})
                if(_wsrc MATCHES "\\.asm$")
                    set_source_files_properties(${_wsrc} PROPERTIES LANGUAGE ASM_MASM)
                endif()
            endforeach()
        endif()
    endif()
    target_include_directories(Qubic PRIVATE ${WAMR_ROOT_DIR}/core/iwasm/include)

    find_package(LibFFI REQUIRED)
    target_link_libraries(
        Qubic
        vmlib
        LibFFI::LibFFI
    )
    message(STATUS "LITE_WASM_SC: WAMR(${WAMR_BUILD_PLATFORM}/${WAMR_BUILD_TARGET}) + libffi")
endif()

if(ADDON_TX_STATUS_REQUEST)
    target_compile_definitions(Qubic PRIVATE ADDON_TX_STATUS_REQUEST=1)
endif()

if(LONG_RUN_LOCAL_TESTNET)
    # Long-run mode is a testnet variant; it implies TESTNET.
    target_compile_definitions(Qubic PRIVATE TESTNET LONG_RUN_LOCAL_TESTNET)
    if(DEFINED LONG_RUN_EPOCH_TICK_CAPACITY)
        target_compile_definitions(Qubic PRIVATE LONG_RUN_EPOCH_TICK_CAPACITY=${LONG_RUN_EPOCH_TICK_CAPACITY}ULL)
    endif()
endif()

# Set specific properties for assembly files to avoid C++ flags being applied
set_source_files_properties(platform/custom_stack.nasm PROPERTIES
    COMPILE_FLAGS ""
    LANGUAGE ASM_MASM
)

# Add additional compiler-specific flags for the main application
if((IS_CLANG OR IS_GCC) AND IS_ARM)
    # ARM receives x86 intrinsics through SIMDe.
    target_compile_options(Qubic PRIVATE
        -D_CONSOLE
        $<$<CONFIG:Release>:-DNDEBUG>
        -fsigned-char
        -fshort-wchar
        -w
    )
elseif(IS_CLANG OR IS_GCC)
    # GCC and Clang need explicit x86 instruction flags.
    target_compile_options(Qubic PRIVATE
        -D_CONSOLE
        $<$<CONFIG:Release>:-DNDEBUG>
        -mrdrnd
        -mbmi
        -mlzcnt
        -fshort-wchar
        -w
    )
elseif(IS_MSVC)
    # Match Qubic.sln's CHAR16 representation and suppress Windows min/max macros.
    set_property(TARGET Qubic PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(Qubic PRIVATE
       /D_CONSOLE
       /DNOMINMAX
       /Zc:wchar_t-
       $<$<CONFIG:Release>:/DNDEBUG>
    )
    target_link_options(Qubic PRIVATE
        $<$<CONFIG:Release>:/SUBSYSTEM:CONSOLE,10.00>
        $<$<CONFIG:Release>:/DEBUG:NONE>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

find_package(Threads REQUIRED)

apply_common_compiler_flags(Qubic)
if(APPLE)
    # ld-prime (Xcode 16) segfaults in ChainedFixupsWriter::buildLinkeditFixups emitting
    # LC_DYLD_CHAINED_FIXUPS for this binary on x86_64; classic bind opcodes link fine.
    if(NOT IS_ARM)
        target_link_options(Qubic PRIVATE -Wl,-no_fixup_chains)
    endif()
    # macOS ld64: no -Bstatic/-Bdynamic, no librt/libdl (in libSystem), no libbacktrace.
    target_link_libraries(Qubic
            platform_common
            platform_efi
            jsoncpp_static drogon fmt::fmt Blosc2::blosc2_static
            Threads::Threads
            z
            m
    )
elseif(WIN32)
    # Windows uses native linker conventions and the WinDbg stacktrace backend.
    target_link_libraries(Qubic
        platform_common
        platform_efi
        jsoncpp_static drogon fmt::fmt Blosc2::blosc2_static
        Threads::Threads
    )
else()
    target_link_libraries(Qubic
        platform_common
        platform_efi
        jsoncpp_static drogon fmt::fmt backtrace_static Blosc2::blosc2_static
        Threads::Threads
        dl
        z
        rt
        m
    )
    # Processor threads run on the libc default thread stack (see Overload::startThread), and processTick
    # alone needs about 132 KB of it. glibc gives 8 MB, musl 128 KB — so a musl build faults in its own
    # prologue on the first tick, with no crash report, the handler having no stack left to run on either.
    target_link_options(Qubic PRIVATE -static -Wl,-z,stack-size=8388608)
endif()

# qubic-cli: standalone debug tool binary (eg. read-entity, read-score-cache).
add_executable(qubic-cli cli/main.cpp)
target_include_directories(qubic-cli PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/lib/platform_common
    ${CMAKE_SOURCE_DIR}/lib/platform_common/edk2_mdepkg/Include
)
target_compile_definitions(qubic-cli PRIVATE NO_UEFI)
if(IS_CLANG)
    target_compile_options(qubic-cli PRIVATE -fshort-wchar -w -mavx -mavx2)
endif()
target_link_libraries(qubic-cli PRIVATE m)

if(ANT_WALKER)
    target_compile_definitions(Qubic PRIVATE ANT_WALKER)
endif()

# Configure linker settings based on compiler
if(IS_MSVC)
    if(NOT LITE_WASM_SC)
        set_target_properties(Qubic PROPERTIES SUFFIX ".efi")
    endif()
elseif(IS_CLANG)
    # Use default linker settings for Clang
endif()
