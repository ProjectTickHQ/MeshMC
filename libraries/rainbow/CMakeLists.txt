cmake_minimum_required(VERSION 3.25)
project(rainbow VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Gui REQUIRED)

set(RAINBOW_SOURCES
src/rainbow.cpp
)

add_definitions(-DRAINBOW_LIBRARY)
add_library(rainbow SHARED ${RAINBOW_SOURCES})
target_include_directories(rainbow PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rainbow PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
