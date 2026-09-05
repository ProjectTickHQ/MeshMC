cmake_minimum_required(VERSION 3.25)
project(Katabasis VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_C_STANDARD_REQUIRED true)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 11)

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core Network REQUIRED)

set( katabasis_PRIVATE
    src/DeviceFlow.cpp
    src/JsonResponse.cpp
    src/JsonResponse.h
    src/PollServer.cpp
    src/Reply.cpp
)

set( katabasis_PUBLIC
    include/katabasis/DeviceFlow.h
    include/katabasis/Globals.h
    include/katabasis/PollServer.h
    include/katabasis/Reply.h
    include/katabasis/RequestParameter.h
)

add_library(Katabasis ${katabasis_PRIVATE} ${katabasis_PUBLIC})
add_library(Katabasis::Katabasis ALIAS Katabasis)
set_target_properties(Katabasis PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Katabasis PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
set_target_properties(Katabasis PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_include_directories(Katabasis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE src include/katabasis)
