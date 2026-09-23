cmake_minimum_required(VERSION 3.15)
project(qubic_core_tests CXX C)

# GoogleTest requires at least C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.16.0
  )
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)


get_filename_component(PROJECT_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(IS_ARM)
    # Prefer the ARM shim over the system x86 CPUID header.
    include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../lib/platform_common/arm_shims)
endif()
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../lib/platform_common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../lib/platform_os)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../lib/platform_efi) # Currently still needed due to various imports
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../src)
include_directories(${PROJECT_ROOT_DIR})

file(COPY data/example_task_bpp9000.bin data/samples_bpp9000.csv data/scores_bpp9000.csv
     data/bpp9000.task data/gt_production.csv data/gt_ant_production.csv
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/data)

add_executable(
  qubic_core_tests
		# Keep first: it overrides MAX_NUMBER_OF_TICKS_PER_EPOCH before including tick_storage.h, and the
		# linker keeps the first COMDAT copy of TickStorage inlines for every TU (oracle tests include it too).
		tick_storage.cpp
		assets.cpp
		wasm_asset_iterator.cpp
   		common_def.cpp
   		contract_core.cpp
		contract_invocation_alignment.cpp
   		contract_gqmprop.cpp
   		contract_msvault.cpp
   		contract_nostromo.cpp
   		contract_qpayhub.cpp
   		contract_qtreat.cpp
   		contract_qbay.cpp
		contract_qbond.cpp
   		contract_qearn.cpp
   		contract_qswap.cpp
   		contract_qutil.cpp
   		contract_qvault.cpp
   		contract_qx.cpp
   		contract_rl.cpp
   		contract_testex.cpp
		contract_escrow.cpp
		contract_quottery.cpp
		contract_qrwa.cpp
		contract_qip.cpp
		contract_qusino.cpp
		ant_colony.cpp
		ant_pending_solutions.cpp
		trit_pack.cpp
		custom_qubic_mining_storage.cpp
		fast_tx_window.cpp
   		file_io.cpp
   		fourq.cpp
   		kangaroo_twelve.cpp
   		m256.cpp
   		math_lib.cpp
   		network_messages.cpp
		pending_txs_pool.cpp
   		platform.cpp
   		qpi.cpp
   		qpi_collection.cpp
   		qpi_date_time.cpp
   		qpi_hash_map.cpp
		qpi_linked_list.cpp
   		revenue.cpp
   		score.cpp
   		score_cache.cpp
   		spectrum.cpp
   		stdlib_impl.cpp
   		demand_zero_pool.cpp
   		time.cpp
   		tx_status_request.cpp
   		uint128.cpp
   		virtual_memory.cpp
   		vote_counter.cpp
		read_write_lock.cpp
		setjump.cpp
		oc_engine.cpp
		oracle_engine.cpp
		windows_lazy_memory.cpp
)

# Fork rollback is Linux-only (fork/pipe/disk-shadow); its test needs the real implementation.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(qubic_core_tests PRIVATE fork_rollback.cpp)
endif()

add_executable(
  qubic_state_tests
    contract_state_pager.cpp
    k12_digest_cache.cpp
    state_memory.cpp
)

set(LOGGING_VM_TEST_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/logging_vm_test_config.h")
set(SPECTRUM_GLOBAL_CONFIG
    "${PROJECT_ROOT_DIR}/src/platform/global_var.h")
if(MSVC)
    target_compile_options(qubic_core_tests PRIVATE
        "/FI${LOGGING_VM_TEST_CONFIG}")
    set_property(SOURCE spectrum.cpp APPEND PROPERTY COMPILE_OPTIONS
        "/FI${SPECTRUM_GLOBAL_CONFIG}")
else()
    target_compile_options(qubic_core_tests PRIVATE
        -include "${LOGGING_VM_TEST_CONFIG}")
    set_property(SOURCE spectrum.cpp APPEND PROPERTY COMPILE_OPTIONS
        -include "${SPECTRUM_GLOBAL_CONFIG}")
endif()

foreach(TEST_TARGET IN ITEMS qubic_core_tests qubic_state_tests)
    apply_test_compiler_flags(${TEST_TARGET})
    if(IS_MSVC)
        target_compile_options(${TEST_TARGET} PRIVATE /DNOMINMAX /Zc:wchar_t- /WX-)
    elseif((IS_CLANG OR IS_GCC) AND IS_ARM)
        target_compile_options(${TEST_TARGET} PRIVATE -fshort-wchar -w)
    elseif(IS_CLANG OR IS_GCC)
        target_compile_options(${TEST_TARGET} PRIVATE
            -mrdrnd -Wno-error -mbmi -mlzcnt -fshort-wchar -w)
    endif()
endforeach()
if(LITE_PARALLEL_SCORE)
    target_compile_definitions(qubic_core_tests PRIVATE LITE_PARALLEL_SCORE)
endif()

target_link_libraries(
  qubic_core_tests PRIVATE
  GTest::gtest_main
  platform_common
  platform_os
  Blosc2::blosc2_static
)

target_link_libraries(
  qubic_state_tests PRIVATE
  GTest::gtest_main
  platform_common
  platform_os
  Blosc2::blosc2_static
)

# Keep Wasm node settings isolated from the default core gtests.
if(LITE_WASM_SC)
    if(NOT TARGET vmlib)
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
        set(WAMR_DISABLE_HW_BOUND_CHECK 1)
        include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
        add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
    endif()

    add_executable(qubic_wasm_tests wasm_contracts.cpp wasm_state_write.cpp wasm_state_journal.cpp wasm_k12_shim.cpp)
    # kangaroo_twelve.h casts bytes to unsigned long long; -Wcast-align -Werror rejects it.
    # MSVC has no such flag and errors on it (D8021), so relax /WX there instead.
    if(MSVC)
        set_source_files_properties(wasm_k12_shim.cpp PROPERTIES COMPILE_OPTIONS "/WX-")
    else()
        set_source_files_properties(wasm_k12_shim.cpp PROPERTIES COMPILE_OPTIONS "-Wno-cast-align;-Wno-comment;-Wno-unused-parameter;-Wno-unused-function")
    endif()
    target_compile_definitions(qubic_wasm_tests PRIVATE
        LITE_WASM_SC
        TESTNET
        TESTNET_LITE_RAM
    )
    apply_test_compiler_flags(qubic_wasm_tests)
    if(MSVC)
        target_compile_definitions(qubic_wasm_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_include_directories(
        qubic_wasm_tests PRIVATE
        ${WAMR_ROOT_DIR}/core/iwasm/include
    )
    target_link_libraries(
        qubic_wasm_tests PRIVATE
        GTest::gtest_main
        vmlib
    )
endif()

include(GoogleTest)
gtest_discover_tests(qubic_core_tests)
gtest_discover_tests(qubic_state_tests)
if(TARGET qubic_wasm_tests)
    gtest_discover_tests(qubic_wasm_tests)
endif()
