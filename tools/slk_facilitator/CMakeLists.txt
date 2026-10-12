cmake_minimum_required(VERSION 3.21)
project(slk_facilitator LANGUAGES CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/slikenet.cmake)

add_executable(slk_facilitator main.cpp)
set_target_properties(slk_facilitator PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
target_link_libraries(slk_facilitator PRIVATE slikenet)
