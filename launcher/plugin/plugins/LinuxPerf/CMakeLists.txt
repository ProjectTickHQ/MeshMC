# MeshMC standalone plugin build bootstrap.
# When this plugin is built from MeshMC's main tree, the parent project
# already provides Qt, MeshMC::SDK, output directories, and signing helpers.
# When this directory is configured as its own repository, provide those
# pieces here so the plugin can be built with only an installed MeshMC SDK.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(LinuxPerf LANGUAGES CXX)

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    set(MeshMC_QT_VERSION_MAJOR "6" CACHE STRING "Major Qt version to build against (5 or 6)")
    set(QT_VERSION_MAJOR "${MeshMC_QT_VERSION_MAJOR}")

    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Gui Network)
    find_package(MeshMC_SDK REQUIRED)

    set(MESHMC_PLUGIN_STAGING_DIR "${CMAKE_BINARY_DIR}/mmcmodules" CACHE PATH
        "Directory where built .mmco plugins are placed")
    set(MMCO_MODULES_DEST_DIR "lib/mmcmodules" CACHE PATH
        "Install directory for .mmco plugins")
    set(MMCO_PLUGIN_DATA_DEST_DIR "share/meshmc/mmcmodules" CACHE PATH
        "Install directory for plugin data files")
endif()

# LinuxPerf — MeshMC plugin (.mmco)
# MangoHud FPS overlay and GameMode performance integration for Minecraft.
#
# Linux-only (UNIX && NOT APPLE).
# Prepends mangohud and/or gamemoderun wrapper commands before launch.
#
# Vendor headers:
#   vendor/gamemode_client.h  — from FeralInteractive/gamemode (BSD-3-Clause)
#   vendor/mangohud_detect.h  — authored for this plugin (MIT)

set(LINUXPERF_SOURCES
    LinuxPerfPlugin.cpp
)

add_library(LinuxPerf MODULE ${LINUXPERF_SOURCES})

target_include_directories(LinuxPerf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
)

target_link_libraries(LinuxPerf PRIVATE
    MeshMC::SDK
    ${CMAKE_DL_LIBS}   # -ldl for dlopen/dlsym used by gamemode_client.h
)

set_target_properties(LinuxPerf PROPERTIES
    PREFIX ""
    SUFFIX ".mmco"
    LIBRARY_OUTPUT_DIRECTORY "${MESHMC_PLUGIN_STAGING_DIR}"
)

foreach(CFG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${CFG}" CFG_UPPER)
    set_target_properties(LinuxPerf PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${MESHMC_PLUGIN_STAGING_DIR}"
    )
endforeach()

install(TARGETS LinuxPerf
    LIBRARY DESTINATION "${MMCO_MODULES_DEST_DIR}"
)

if(MeshMC_PLUGIN_SIGN_ALL)
    mmco_sign_plugin(LinuxPerf)
endif()
