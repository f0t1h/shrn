cmake_minimum_required(VERSION 3.20)
project(shrn VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

add_library(shrn INTERFACE)
add_library(shrn::shrn ALIAS shrn)
target_compile_features(shrn INTERFACE cxx_std_20)
target_include_directories(shrn INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

find_package(Threads REQUIRED)
target_link_libraries(shrn INTERFACE Threads::Threads)


# Install / package config.
include(CMakePackageConfigHelpers)

install(TARGETS shrn EXPORT shrnTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT shrnTargets
    NAMESPACE shrn::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shrn)

configure_package_config_file(cmake/shrnConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/shrnConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shrn)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/shrnConfigVersion.cmake
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/shrnConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/shrnConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shrn)

# Tests when building shrn directly.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(shrn_tests tests/test_shrn.cpp)
        target_link_libraries(shrn_tests PRIVATE shrn::shrn)
        target_compile_options(shrn_tests PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME shrn_tests COMMAND shrn_tests)
    endif()
endif()
