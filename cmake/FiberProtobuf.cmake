include_guard()

# fiber_proto_library(<name> <proto_file>...)
#
# Generates LITE_RUNTIME C++ from .proto files (expected under
# ${CMAKE_CURRENT_SOURCE_DIR}/proto) into ${CMAKE_BINARY_DIR}/gen/proto/<name>
# and exposes a static library target `fiber_proto_<name>` that links
# protobuf::libprotobuf-lite. The generated code relies on the .proto option
# `optimize_for = LITE_RUNTIME;` (protoc has no cpp_out flag for lite).
function(fiber_proto_library name)
    fiber_prepare_protobuf_target()

    set(proto_files ${ARGN})
    set(proto_root "${CMAKE_CURRENT_SOURCE_DIR}/proto")
    set(out_dir "${CMAKE_BINARY_DIR}/gen/proto/${name}")
    file(MAKE_DIRECTORY "${out_dir}")

    set(generated_srcs)
    set(dep_files)
    foreach(pf IN LISTS proto_files)
        get_filename_component(pf_name "${pf}" NAME_WLE)
        list(APPEND generated_srcs "${out_dir}/${pf_name}.pb.cc")
        list(APPEND dep_files "${proto_root}/${pf}")
    endforeach()

    add_custom_command(
        OUTPUT ${generated_srcs}
        COMMAND ${FIBER_PROTOC_TARGET}
                --proto_path=${proto_root}
                --cpp_out=${out_dir}
                ${proto_files}
        DEPENDS ${FIBER_PROTOC_TARGET} ${dep_files}
        COMMENT "protoc LITE codegen: ${name}"
        VERBATIM)

    add_library(fiber_proto_${name} STATIC ${generated_srcs})
    target_include_directories(fiber_proto_${name} PUBLIC "${out_dir}")
    target_link_libraries(fiber_proto_${name} PUBLIC protobuf::libprotobuf-lite)
    set_target_properties(fiber_proto_${name} PROPERTIES FOLDER "proto")
    if(FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_proto_${name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()
