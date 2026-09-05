# MeshMC standalone plugin build bootstrap.
# When this plugin is built from MeshMC's main tree, the parent project
# already provides Qt, MeshMC::SDK, output directories, and signing helpers.
# When this directory is configured as its own repository, provide those
# pieces here so the plugin can be built with only an installed MeshMC SDK.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(ErrorOracle LANGUAGES CXX)

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

# SPDX-FileCopyrightText: 2026 Project Tick
# SPDX-License-Identifier: Apache-2.0
#
# ErrorOracle — MeshMC plugin (.mmco)
#
# LLM-free crash / log analyser. Two layers:
#
#   1. A built-in rule pack (rules/*.json) shipped with the plugin
#      mapping regex patterns to remediation advice. Loaded once at
#      init time.
#
#   2. A *learning* layer that tracks (rule, instance) -> outcome
#      pairs over time, persists them under <plugin_data>/learn.json,
#      and re-ranks rule suggestions for an instance based on what
#      worked last time. Pure local statistics, no network.
#
# Users can drop additional rule packs into
# <plugin_data>/userrules/*.json without recompiling — the rule
# engine watches that directory at init time and reloads when the
# user clicks the "Reload rules" button.

set(ERR_SOURCES
    ErrorOraclePlugin.cpp
    RuleEngine.h
    RuleEngine.cpp
    LogIngester.h
    LogIngester.cpp
    LearningStore.h
    LearningStore.cpp
    AnalysisPage.h
    AnalysisPage.cpp
)

add_library(ErrorOracle MODULE ${ERR_SOURCES})

target_link_libraries(ErrorOracle PRIVATE
    MeshMC::SDK
)

target_include_directories(ErrorOracle PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

set_target_properties(ErrorOracle PROPERTIES
    PREFIX ""
    SUFFIX ".mmco"
    LIBRARY_OUTPUT_DIRECTORY "${MESHMC_PLUGIN_STAGING_DIR}"
)

foreach(CFG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${CFG}" CFG_UPPER)
    set_target_properties(ErrorOracle PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${MESHMC_PLUGIN_STAGING_DIR}"
    )
endforeach()

# Install the bundled rule pack as PLUGIN DATA, not next to the
# .mmco. On macOS the .mmco lives under Contents/PlugIns/mmcmodules,
# which Apple's codesign treats as nested-code territory; dropping
# JSON files there causes
#   "code object is not signed at all
#    In subcomponent: .../PlugIns/mmcmodules/ErrorOracle/rules/jvm.json"
# at signing time. MMCO_PLUGIN_DATA_DEST_DIR resolves to:
#   Linux/Windows -> <install>/mmcmodules        (legacy, next to .mmco)
#   macOS         -> MeshMC.app/Contents/Resources/MMCOPluginData
# The runtime lookup chain in ErrorOraclePlugin::builtInRulesDir
# probes the platform-specific location first, then falls back to
# the legacy "next to the .mmco" path so older installs and ad-hoc
# developer builds keep working.
install(DIRECTORY rules
    DESTINATION "${MMCO_PLUGIN_DATA_DEST_DIR}/ErrorOracle"
    FILES_MATCHING PATTERN "*.json"
)

# Stage rule files next to the .mmco in the build tree.
file(GLOB _eo_rule_files "${CMAKE_CURRENT_SOURCE_DIR}/rules/*.json")
foreach(_rule ${_eo_rule_files})
    file(COPY "${_rule}"
         DESTINATION "${MESHMC_PLUGIN_STAGING_DIR}/ErrorOracle/rules")
endforeach()

install(TARGETS ErrorOracle
    LIBRARY DESTINATION "${MMCO_MODULES_DEST_DIR}"
)

if(MeshMC_PLUGIN_SIGN_ALL)
    mmco_sign_plugin(ErrorOracle)
endif()
