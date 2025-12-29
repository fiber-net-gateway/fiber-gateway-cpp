cmake_minimum_required(VERSION 4.1)
project(fiber)

set(CMAKE_CXX_STANDARD 23)
file(GLOB_RECURSE CPP_SRC_LIST ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)


add_executable(fiber ${CPP_SRC_LIST})
