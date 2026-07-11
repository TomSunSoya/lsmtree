find_program(CLANG_FORMAT_EXECUTABLE
        NAMES clang-format
        HINTS /opt/homebrew/opt/llvm/bin)

set(LSMTREE_FORMAT_COMMAND "${PROJECT_SOURCE_DIR}/tools/clang-format.sh")
if(CLANG_FORMAT_EXECUTABLE)
    set(LSMTREE_FORMAT_COMMAND
            "${CMAKE_COMMAND}" -E env "CLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/clang-format.sh")
endif()

add_custom_target(format
        COMMAND ${LSMTREE_FORMAT_COMMAND} format
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Formatting C and C++ sources"
        VERBATIM
        USES_TERMINAL)

add_custom_target(format-check
        COMMAND ${LSMTREE_FORMAT_COMMAND} check
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Checking C and C++ source formatting"
        VERBATIM
        USES_TERMINAL)
