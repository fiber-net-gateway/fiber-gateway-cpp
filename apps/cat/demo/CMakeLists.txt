fiber_add_app(cat_demo
    OUTPUT_NAME "cat-demo"
    SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/cat_demo.cpp"
    LIBRARIES
        fiber::cat)
