include_guard(GLOBAL)

function(fiber_add_runtime_executable target_name)
    set(options)
    set(one_value_args OUTPUT_NAME RUNTIME_OUTPUT_DIRECTORY IDE_FOLDER)
    set(multi_value_args SOURCES LIBRARIES INCLUDE_DIRS LINK_OPTIONS)
    cmake_parse_arguments(FIBER "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (NOT FIBER_SOURCES)
        message(FATAL_ERROR "fiber_add_runtime_executable(${target_name}) requires SOURCES")
    endif()

    add_executable(${target_name} ${FIBER_SOURCES})
    target_link_libraries(${target_name} PRIVATE fiber_lib ${FIBER_LIBRARIES})

    if (FIBER_INCLUDE_DIRS)
        target_include_directories(${target_name} PRIVATE ${FIBER_INCLUDE_DIRS})
    endif()
    if (FIBER_LINK_OPTIONS)
        target_link_options(${target_name} PRIVATE ${FIBER_LINK_OPTIONS})
    endif()
    if (FIBER_OUTPUT_NAME)
        set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${FIBER_OUTPUT_NAME}")
    endif()
    if (FIBER_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${FIBER_RUNTIME_OUTPUT_DIRECTORY}")
    endif()
    if (FIBER_IDE_FOLDER)
        set_target_properties(${target_name} PROPERTIES FOLDER "${FIBER_IDE_FOLDER}")
    endif()

    fiber_link_runtime_allocator(${target_name})
    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

function(fiber_add_example example_name)
    fiber_add_runtime_executable(${example_name}
        IDE_FOLDER "example"
        ${ARGN})
endfunction()

function(fiber_add_app app_name)
    set(options)
    set(one_value_args OUTPUT_NAME)
    set(multi_value_args SOURCES LIBRARIES INCLUDE_DIRS LINK_OPTIONS)
    cmake_parse_arguments(FIBER_APP "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(target_name "fiber_app_${app_name}")
    set(output_name "${app_name}")
    if (FIBER_APP_OUTPUT_NAME)
        set(output_name "${FIBER_APP_OUTPUT_NAME}")
    endif()

    fiber_add_runtime_executable(${target_name}
        OUTPUT_NAME "${output_name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps"
        IDE_FOLDER "apps"
        SOURCES ${FIBER_APP_SOURCES}
        LIBRARIES ${FIBER_APP_LIBRARIES}
        INCLUDE_DIRS ${FIBER_APP_INCLUDE_DIRS}
        LINK_OPTIONS ${FIBER_APP_LINK_OPTIONS})
endfunction()
