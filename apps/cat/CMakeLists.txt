add_library(fiber_cat STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/Cat.cpp")
add_library(fiber::cat ALIAS fiber_cat)

target_include_directories(fiber_cat
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(fiber_cat PUBLIC fiber_lib)
set_target_properties(fiber_cat PROPERTIES FOLDER "apps/cat")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET fiber_cat PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

fiber_add_app(cat_hello
    SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp"
    LIBRARIES
        fiber::cat)
