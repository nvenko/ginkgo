cmake_minimum_required(VERSION 3.16)
project(GinkgoSubdirTest LANGUAGES CXX)
file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/ginkgo" SYMBOLIC)

add_subdirectory("${CMAKE_CURRENT_BINARY_DIR}/ginkgo")

add_executable(test_subdir ../test_install/test_install.cpp)
target_link_libraries(test_subdir PRIVATE Ginkgo::ginkgo)
