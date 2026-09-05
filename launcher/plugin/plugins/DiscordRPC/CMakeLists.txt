# MeshMC standalone plugin build bootstrap.
# When this plugin is built from MeshMC's main tree, the parent project
# already provides Qt, MeshMC::SDK, output directories, and signing helpers.
# When this directory is configured as its own repository, provide those
# pieces here so the plugin can be built with only an installed MeshMC SDK.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(DiscordRPC LANGUAGES CXX)

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

# DiscordRPC — MeshMC plugin (.mmco)
#
# Discord Rich Presence integration. Talks to the local Discord client
# over the documented IPC socket (UNIX socket on Linux/macOS, named
# pipe on Windows). No network access required.
#
# Vendor:
#   vendor/discord_ipc/  — Qt-only minimal Discord IPC client (MIT)

set(DISCORDRPC_SOURCES
    DiscordRPCPlugin.cpp
    vendor/discord_ipc/discord_ipc.h
    vendor/discord_ipc/discord_ipc.cpp
)

add_library(DiscordRPC MODULE ${DISCORDRPC_SOURCES})

target_link_libraries(DiscordRPC PRIVATE
    MeshMC::SDK
    Qt${QT_VERSION_MAJOR}::Network            # QLocalSocket lives here
)

target_include_directories(DiscordRPC PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
)

set_target_properties(DiscordRPC PROPERTIES
    PREFIX ""
    SUFFIX ".mmco"
    LIBRARY_OUTPUT_DIRECTORY "${MESHMC_PLUGIN_STAGING_DIR}"
)

foreach(CFG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${CFG}" CFG_UPPER)
    set_target_properties(DiscordRPC PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${MESHMC_PLUGIN_STAGING_DIR}"
    )
endforeach()

install(TARGETS DiscordRPC
    LIBRARY DESTINATION "${MMCO_MODULES_DEST_DIR}"
)

if(MeshMC_PLUGIN_SIGN_ALL)
    mmco_sign_plugin(DiscordRPC)
endif()
