include_guard()

# Test-only zlib 1.3.2 reference objects.
#
# Builds the unmodified upstream sources under
# tests/support/third_party/zlib_1_3_2 into an OBJECT library whose externally
# visible symbols are renamed to fiber_test_zlib_* via a force-included prefix
# header. Together with the ZlibReference.cpp adapter this gives the test
# executables an independent inflate/deflate implementation. The target is
# created only when tests are built and must never be linked into production.

# Resolved at include time: inside the function CMAKE_CURRENT_LIST_DIR is the
# caller's directory, not this file's.
set(FIBER_ZLIB_REFERENCE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(fiber_add_zlib_reference)
    if (TARGET fiber_zlib_reference)
        return()
    endif()

    set(FIBER_ZLIB_REFERENCE_DIR "${FIBER_ZLIB_REFERENCE_CMAKE_DIR}/../tests/support/third_party/zlib_1_3_2")
    set(FIBER_TESTS_DIR "${FIBER_ZLIB_REFERENCE_CMAKE_DIR}/../tests")
    set(FIBER_TEST_SUPPORT_DIR "${FIBER_TESTS_DIR}/support")
    set(FIBER_ZLIB_REFERENCE_PREFIX "${FIBER_ZLIB_REFERENCE_DIR}/fiber_test_zlib_prefix.h")

    add_library(fiber_zlib_reference OBJECT
        "${FIBER_ZLIB_REFERENCE_DIR}/adler32.c"
        "${FIBER_ZLIB_REFERENCE_DIR}/crc32.c"
        "${FIBER_ZLIB_REFERENCE_DIR}/deflate.c"
        "${FIBER_ZLIB_REFERENCE_DIR}/inffast.c"
        "${FIBER_ZLIB_REFERENCE_DIR}/inflate.c"
        "${FIBER_ZLIB_REFERENCE_DIR}/inftrees.c"
        "${FIBER_ZLIB_REFERENCE_DIR}/trees.c"
        "${FIBER_ZLIB_REFERENCE_DIR}/zutil.c"
        "${FIBER_TEST_SUPPORT_DIR}/ZlibReference.cpp")
    set_target_properties(fiber_zlib_reference PROPERTIES
        FOLDER "fiber/tests"
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 17
        C_STANDARD_REQUIRED ON)
    target_include_directories(fiber_zlib_reference PRIVATE
        "${FIBER_ZLIB_REFERENCE_DIR}"
        "${FIBER_TESTS_DIR}")
    target_compile_options(fiber_zlib_reference PRIVATE
        # Rename every external zlib symbol; see fiber_test_zlib_prefix.h.
        # Applies to both the C sources and the ZlibReference.cpp adapter.
        "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang,GNU>:-include${FIBER_ZLIB_REFERENCE_PREFIX}>"
        "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-include${FIBER_ZLIB_REFERENCE_PREFIX}>"
        # Upstream sources, upstream diagnostics policy: keep them quiet.
        "$<$<COMPILE_LANGUAGE:C>:-w>")
    # Link helper: the objects plus the adapter's header path, without ever
    # propagating the reference include directory or prefix macros.
    add_library(fiber_zlib_reference_objects INTERFACE)
    target_sources(fiber_zlib_reference_objects INTERFACE
        $<TARGET_OBJECTS:fiber_zlib_reference>)
    target_include_directories(fiber_zlib_reference_objects INTERFACE
        "${FIBER_TESTS_DIR}")
endfunction()
