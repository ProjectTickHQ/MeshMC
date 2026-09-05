# Example CMakeLists.txt for an out-of-tree MeshMC plugin (.mmco)
#
# Prerequisites:
#   1. Install MeshMC with SDK headers:
#        cmake --install <build-dir> --prefix /usr/local
#   2. Install Qt development packages (Core, Widgets, Gui) for the same Qt
#      major version MeshMC itself was built against (Qt 5 or Qt 6)
#
# Build:
#   cmake -B build -DCMAKE_PREFIX_PATH="/usr/local;/usr/lib/qt6"
#   cmake --build build
#
# Install:
#   cp build/MyPlugin.mmco ~/.local/lib/mmcmodules/

cmake_minimum_required(VERSION 3.21)
project(MyPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Must match the Qt major version MeshMC and its SDK were built against;
# the exported MeshMC::SDK target names Qt<major>::* targets concretely.
# The installed SDK config reports what it was built with in
# MeshMC_SDK_QT_VERSION_MAJOR.
set(MeshMC_QT_VERSION_MAJOR "6" CACHE STRING "Major Qt version to build against (5 or 6)")
set(QT_VERSION_MAJOR "${MeshMC_QT_VERSION_MAJOR}")

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Gui)
find_package(MeshMC_SDK REQUIRED)

add_library(MyPlugin MODULE
    src/MyPlugin.cpp
)

target_link_libraries(MyPlugin PRIVATE
    MeshMC::SDK
)

set_target_properties(MyPlugin PROPERTIES
    PREFIX ""
    SUFFIX ".mmco"
)
