if (NOT DEFINED INPUT_FILE OR NOT EXISTS "${INPUT_FILE}")
    message(FATAL_ERROR "EmbedBinary.cmake requires an existing INPUT_FILE")
endif()
if (NOT DEFINED OUTPUT_FILE OR OUTPUT_FILE STREQUAL "")
    message(FATAL_ERROR "EmbedBinary.cmake requires OUTPUT_FILE")
endif()

file(READ "${INPUT_FILE}" FIBER_EMBEDDED_HEX HEX)
string(LENGTH "${FIBER_EMBEDDED_HEX}" FIBER_EMBEDDED_HEX_LENGTH)
math(EXPR FIBER_EMBEDDED_SIZE "${FIBER_EMBEDDED_HEX_LENGTH} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FIBER_EMBEDDED_BYTES "${FIBER_EMBEDDED_HEX}")

get_filename_component(FIBER_EMBEDDED_OUTPUT_DIR "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${FIBER_EMBEDDED_OUTPUT_DIR}")
file(WRITE "${OUTPUT_FILE}"
"#ifndef FIBER_SCRIPT_JIT_OPERATOR_BITCODE_H\n"
"#define FIBER_SCRIPT_JIT_OPERATOR_BITCODE_H\n\n"
"#include <cstddef>\n\n"
"namespace fiber::script::jit::llvm_detail {\n\n"
"inline constexpr unsigned char kOperatorHelperBitcode[] = {${FIBER_EMBEDDED_BYTES}};\n"
"inline constexpr std::size_t kOperatorHelperBitcodeSize = ${FIBER_EMBEDDED_SIZE};\n\n"
"} // namespace fiber::script::jit::llvm_detail\n\n"
"#endif // FIBER_SCRIPT_JIT_OPERATOR_BITCODE_H\n")
