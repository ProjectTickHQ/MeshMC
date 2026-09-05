cmake_minimum_required(VERSION 3.25)
project(LocalPeer VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_AUTOMOC ON)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Network REQUIRED)

set(SINGLE_SOURCES
src/LocalPeer.cpp
src/LockedFile.cpp
src/LockedFile.h
include/LocalPeer.h
)

if(UNIX)
    list(APPEND SINGLE_SOURCES
        src/LockedFile_unix.cpp
    )
endif()

if(WIN32)
    list(APPEND SINGLE_SOURCES
        src/LockedFile_win.cpp
    )
endif()

add_library(LocalPeer ${SINGLE_SOURCES})
set_target_properties(LocalPeer PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(LocalPeer::LocalPeer ALIAS LocalPeer)
target_include_directories(LocalPeer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/LocalPeer>)
target_link_libraries(LocalPeer PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
