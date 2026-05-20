cmake_minimum_required(VERSION 3.15)
project(qubic_core CXX ASM_NASM)

# GoogleTest requires at least C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libraries" FORCE)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a") # Only look for static archives

# Enable FetchContent module
include(FetchContent)
include(ExternalProject)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/lib/)

set(ASM_FILE "${CMAKE_CURRENT_SOURCE_DIR}/platform/custom_stack.nasm")
set(ASM_OBJ "${CMAKE_CURRENT_BINARY_DIR}/custom_stack.obj")

add_custom_command(
    OUTPUT ${ASM_OBJ}
    COMMAND ${NASM_EXECUTABLE} -f elf64 ${ASM_FILE} -o ${ASM_OBJ}
    DEPENDS ${ASM_FILE}
    COMMENT "Assembling ${ASM_FILE}"
)

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

# Define the shared library (matches Visual Studio DynamicLibrary configuration)
add_executable(
  Qubic
  qubic.cpp
  ${ASM_OBJ}
)

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

if(NO_ENABLE_QUBIC_LOGGING_EVENT)
    target_compile_definitions(Qubic PRIVATE NO_ENABLE_QUBIC_LOGGING_EVENT)
endif()

if(TESTNET)
    target_compile_definitions(Qubic PRIVATE TESTNET)
endif()

if(TESTNET_PREFILL_QUS)
    target_compile_definitions(Qubic PRIVATE TESTNET_PREFILL_QUS)
endif()

# Set specific properties for assembly files to avoid C++ flags being applied
set_source_files_properties(platform/custom_stack.nasm PROPERTIES
    COMPILE_FLAGS ""
    LANGUAGE ASM_MASM
)

# Add additional compiler-specific flags for the main application
if(IS_CLANG)
    # Clang-specific flags
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
    # MSVC-specific flags
    target_compile_options(Qubic PRIVATE
       /D_CONSOLE
       $<$<CONFIG:Release>:/DNDEBUG>
    )
endif()

find_package(Threads REQUIRED)

apply_common_compiler_flags(Qubic)
target_link_libraries(Qubic
        platform_common
        platform_efi
        -Wl,-Bstatic
        jsoncpp_static drogon fmt::fmt backtrace_static
        -Wl,-Bdynamic
        Threads::Threads
        dl
        z
        zstd
        rt
        m
)

# Configure linker settings based on compiler
if(IS_MSVC)
    # MSVC-specific linker settings
    set_target_properties(Qubic PROPERTIES SUFFIX ".efi")
    
elseif(IS_CLANG)
    # Use default linker settings for Clang
endif()

