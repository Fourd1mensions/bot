cmake_minimum_required(VERSION 3.15)
project(osu-pp-calculator VERSION 1.0.0 LANGUAGES CXX)

# Header-only library
add_library(osu-pp-calculator INTERFACE)
add_library(osupp::calculator ALIAS osu-pp-calculator)

target_include_directories(osu-pp-calculator
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(osu-pp-calculator INTERFACE cxx_std_17)

# Installation rules
include(GNUInstallDirs)

install(TARGETS osu-pp-calculator
    EXPORT osu-pp-calculator-targets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT osu-pp-calculator-targets
    FILE osu-pp-calculator-targets.cmake
    NAMESPACE osupp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osu-pp-calculator
)

# Package config
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/osu-pp-calculator-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/osu-pp-calculator-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osu-pp-calculator
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/osu-pp-calculator-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/osu-pp-calculator-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/osu-pp-calculator-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osu-pp-calculator
)
