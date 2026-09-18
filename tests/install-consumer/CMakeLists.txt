cmake_minimum_required(VERSION 3.20)
project(shrn_install_consumer LANGUAGES CXX)

find_package(shrn CONFIG REQUIRED)

add_executable(shrn_install_consumer main.cpp)
target_link_libraries(shrn_install_consumer PRIVATE shrn::shrn)

enable_testing()
add_test(NAME shrn_install_consumer COMMAND shrn_install_consumer)
