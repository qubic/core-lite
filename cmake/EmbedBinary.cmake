if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "INPUT_FILE, OUTPUT_FILE and SYMBOL are required")
endif()
if(NOT EXISTS "${INPUT_FILE}")
    message(FATAL_ERROR "Missing binary asset: ${INPUT_FILE}")
endif()

file(READ "${INPUT_FILE}" _hex HEX)
file(SIZE "${INPUT_FILE}" _size)
file(SHA256 "${INPUT_FILE}" _sha256)
get_filename_component(_name "${INPUT_FILE}" NAME)

# 16 bytes per line, formatted in whole-string passes: a per-line loop is quadratic on a large asset.
string(REGEX REPLACE "(................................)" "\\1\n" _hex "${_hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
string(STRIP "${_hex}" _hex)
string(REPLACE "\n" "\n    " _hex "${_hex}")

set(_contents
"// AUTO-GENERATED from data/${_name} by cmake/EmbedBinary.cmake.
// SHA-256: ${_sha256}
#pragma once

alignas(64) inline constexpr unsigned char ${SYMBOL}_BYTES[] = {
    ${_hex}
};
inline constexpr unsigned long long ${SYMBOL}_SIZE = sizeof(${SYMBOL}_BYTES);
static_assert(${SYMBOL}_SIZE == ${_size}ULL);
")

file(WRITE "${OUTPUT_FILE}" "${_contents}")

if(DEFINED EXPECTED_FILE)
    file(READ "${EXPECTED_FILE}" _expected)
    string(REPLACE "\r\n" "\n" _expected "${_expected}")
    if(NOT "${_expected}" STREQUAL "${_contents}")
        message(FATAL_ERROR
            "${EXPECTED_FILE} is stale; regenerate it from ${INPUT_FILE} with cmake/EmbedBinary.cmake")
    endif()
endif()
