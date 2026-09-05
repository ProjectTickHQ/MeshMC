cmake_minimum_required(VERSION 3.25)
project(classparser VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_AUTOMOC ON)

######## Check endianness ########
include(TestBigEndian)
test_big_endian(BIGENDIAN)
if(${BIGENDIAN})
    add_definitions(-DMULTIMC_BIG_ENDIAN)
endif(${BIGENDIAN})

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core REQUIRED)

# When built as part of the MeshMC tree the top-level build has already
# resolved libarchive (from vcpkg, or from the system package), so only a
# standalone build has to look for it.
if(NOT TARGET LibArchive::LibArchive)
    find_package(LibArchive REQUIRED)
endif()

set(CLASSPARSER_HEADERS
include/classparser_config.h
include/classparser.h
src/annotations.h
src/classfile.h
src/constants.h
src/errors.h
src/javaendian.h
src/membuffer.h
)

set(CLASSPARSER_SOURCES
src/classparser.cpp
src/annotations.cpp
)

add_definitions(-DCLASSPARSER_LIBRARY)

add_library(classparser ${CLASSPARSER_SOURCES} ${CLASSPARSER_HEADERS})
set_target_properties(classparser PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(classparser
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/classparser>)
target_link_libraries(classparser PRIVATE LibArchive::LibArchive Qt${QT_VERSION_MAJOR}::Core)
