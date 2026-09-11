cmake_minimum_required(VERSION 3.20)
project(shrn VERSION 0.1.0 LANGUAGES CXX)

option(SHRN_USE_ZLIB "Link zlib for gzip-aware helpers when found" ON)

add_library(shrn INTERFACE)
add_library(shrn::shrn ALIAS shrn)
target_compile_features(shrn INTERFACE cxx_std_20)
target_include_directories(shrn INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

set(SHRN_WITH_ZLIB OFF)
if(SHRN_USE_ZLIB)
    find_package(ZLIB)
    if(ZLIB_FOUND)
        target_link_libraries(shrn INTERFACE ZLIB::ZLIB)
        set(SHRN_WITH_ZLIB ON)
    endif()
endif()
if(NOT SHRN_WITH_ZLIB)
    # The header auto-detects <zlib.h>; make sure it cannot pick it up unlinked.
    target_compile_definitions(shrn INTERFACE SHRN_NO_ZLIB=1)
endif()

# ---------------------------------------------------------------------------
# Install / package config.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
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
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/shrnConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/shrnConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shrn)

# ---------------------------------------------------------------------------
# Tests: built only when shrn is the top-level project.
# Two binaries: `native` (C++23 when the compiler has it, so std::expected is
# exercised) and `polyfill` (C++20, SHRN_FORCE_POLYFILL).
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        foreach(variant IN ITEMS native polyfill)
            set(target shrn_tests_${variant})
            add_executable(${target} tests/test_shrn.cpp)
            target_link_libraries(${target} PRIVATE shrn::shrn)
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
            if(variant STREQUAL polyfill)
                target_compile_definitions(${target} PRIVATE SHRN_FORCE_POLYFILL=1)
            elseif(cxx_std_23 IN_LIST CMAKE_CXX_COMPILE_FEATURES)
                target_compile_features(${target} PRIVATE cxx_std_23)
            endif()
            add_test(NAME ${target} COMMAND ${target})
        endforeach()
    endif()
endif()
