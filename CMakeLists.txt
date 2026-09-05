cmake_minimum_required(VERSION 3.24)
# SPDX-FileCopyrightText: 2026 Project Tick
# SPDX-FileContributor: Project Tick
# SPDX-License-Identifier: Apache-2.0

# Required for CMAKE_MSVC_DEBUG_INFORMATION_FORMAT to work (avoids /Zi PDB conflicts with sccache)
cmake_policy(SET CMP0141 NEW)

# Required so CMAKE_MSVC_RUNTIME_LIBRARY controls the CRT (/MD vs /MDd) instead
# of it being baked into CMAKE_CXX_FLAGS. The CRT is pinned to /MD in the
# if(MSVC) block below, to match how the vcpkg dependencies are built; see the
# note there. Must be set before project().
cmake_policy(SET CMP0091 NEW)

# Never search our own install prefix when resolving find_package().
#
# The presets install into <source>/install, and project() would otherwise add
# CMAKE_INSTALL_PREFIX to CMAKE_SYSTEM_PREFIX_PATH — so after one
# `cmake --install`, the next configure starts finding our own installed
# artifacts as if they were system packages. That is how a half-installed ECM
# (only ECMConfig.cmake landed under install/share/ECM, none of the modules)
# made find_package(ECM) succeed against a broken prefix and took the whole
# configure down with "could not find ECMUseFindModules.cmake".
#
# Nothing we legitimately depend on lives in our own install tree, so refusing
# to look there removes that entire class of self-poisoning. Must be set before
# project(), which is when CMAKE_SYSTEM_PREFIX_PATH is built.
set(CMAKE_FIND_NO_INSTALL_PREFIX TRUE)

project(MeshMC)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ECM NO_MODULE REQUIRED)
set(CMAKE_MODULE_PATH "${ECM_MODULE_PATH};${CMAKE_MODULE_PATH}")

include(CTest)
include(ECMAddTests)
if(BUILD_TESTING)
    enable_testing()
endif()

string(COMPARE EQUAL "${CMAKE_SOURCE_DIR}" "${CMAKE_BUILD_DIR}" IS_IN_SOURCE_BUILD)
if(IS_IN_SOURCE_BUILD)
    message(FATAL_ERROR "You are building MeshMC in-source. Please separate the build tree from the source tree.")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_HOST_SYSTEM_VERSION MATCHES ".*[Mm]icrosoft.*" OR
        CMAKE_HOST_SYSTEM_VERSION MATCHES ".*WSL.*"
    )
        message(FATAL_ERROR "Building MeshMC is not supported in Linux-on-Windows distributions.")
    endif()
endif()

set(MeshMC_COMPILER_NAME ${CMAKE_CXX_COMPILER_ID})
set(MeshMC_COMPILER_VERSION ${CMAKE_CXX_COMPILER_VERSION})
set(MeshMC_COMPILER_TARGET_SYSTEM ${CMAKE_SYSTEM_NAME})
set(MeshMC_COMPILER_TARGET_SYSTEM_VERSION ${CMAKE_SYSTEM_VERSION})
set(MeshMC_COMPILER_TARGET_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR})

##################################### Set CMake options #####################################
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/;${CMAKE_MODULE_PATH}")

# Output all executables and shared libs in the main build folder, not in subfolders.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(UNIX)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
endif()
set(CMAKE_JAVA_TARGET_OUTPUT_DIR ${PROJECT_BINARY_DIR}/jars)

######## Set compiler flags ########
set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_C_STANDARD_REQUIRED true)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23)

include(GenerateExportHeader)
if(MSVC)
    set(CMAKE_CXX_FLAGS " /W4 ${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /DQT_NO_DEPRECATED_WARNINGS=Y")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release "")
else()
    # FIXME: readd -Werror flag
    set(CMAKE_CXX_FLAGS " -Wall -pedantic -Wno-deprecated-declarations -fstack-protector-strong --param=ssp-buffer-size=4 -O3 -D_FORTIFY_SOURCE=2 ${CMAKE_CXX_FLAGS}")
    if(UNIX AND APPLE)
        set(CMAKE_CXX_FLAGS " -stdlib=libc++ ${CMAKE_CXX_FLAGS}")
    endif()
    # FIXME: readd this: set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Werror=return-type")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DQT_NO_DEPRECATED_WARNINGS=Y")
endif()

include(ECMQtDeclareLoggingCategory)

option(ENABLE_LTO "Enable Link Time Optimization" off)
option(MeshMC_DISABLE_JAVA_DOWNLOADER "Disable the built-in Java downloader feature" OFF)
option(MeshMC_ENABLE_CLANG_TIDY "Run clang-tidy during C/C++ compilation" OFF)
option(MeshMC_PLUGINS "Build plugins" OFF)
option(MeshMC_STAGING_PLUGINS "Build Staging plugins" OFF)

find_program(MeshMC_CLANG_TIDY_EXECUTABLE NAMES clang-tidy clang-tidy-19 OPTIONAL)
if(MeshMC_ENABLE_CLANG_TIDY)
    if(MeshMC_CLANG_TIDY_EXECUTABLE)
        set(CMAKE_C_CLANG_TIDY
            "${MeshMC_CLANG_TIDY_EXECUTABLE};--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
        set(CMAKE_CXX_CLANG_TIDY
            "${MeshMC_CLANG_TIDY_EXECUTABLE};--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
        message(STATUS "clang-tidy enabled: ${MeshMC_CLANG_TIDY_EXECUTABLE}")
    else()
        message(WARNING "MeshMC_ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
    endif()
endif()

if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)

    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        if(CMAKE_BUILD_TYPE)
            if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
                message(STATUS "IPO / LTO enabled")
            else()
                message(STATUS "Not enabling IPO / LTO on debug builds")
            endif()
        else()
            message(STATUS "IPO / LTO will only be enabled for release builds")
        endif()
    else()
        message(STATUS "IPO / LTO not supported: <${ipo_error}>")
    endif()
endif()

################################ 3rd Party Libs ################################

# Find the required Qt parts
#
# MeshMC supports building against either Qt 5 or Qt 6. Every subdirectory
# refers to Qt through the ${QT_VERSION_MAJOR} indirection set up here rather
# than hardcoding Qt5::/Qt6::, so this is the single place that decides which
# major version the whole build uses.
set(MeshMC_QT_VERSION_MAJOR "6" CACHE STRING "Major Qt version to build against (5 or 6)")
set_property(CACHE MeshMC_QT_VERSION_MAJOR PROPERTY STRINGS "5" "6")

if(NOT MeshMC_QT_VERSION_MAJOR STREQUAL "5" AND NOT MeshMC_QT_VERSION_MAJOR STREQUAL "6")
    message(FATAL_ERROR "MeshMC_QT_VERSION_MAJOR must be either 5 or 6. Got: ${MeshMC_QT_VERSION_MAJOR}")
endif()

set(QT_VERSION_MAJOR "${MeshMC_QT_VERSION_MAJOR}")

set(MeshMC_QT_COMPONENTS
    Core
    Widgets
    Concurrent
    Network
    NetworkAuth
    Test
    Xml
    OpenGL
)

if(QT_VERSION_MAJOR EQUAL 6)
    # Qt 6 split QOpenGLWidget out of QtWidgets into its own module.
    list(APPEND MeshMC_QT_COMPONENTS OpenGLWidgets)
    set(MeshMC_QT_MINIMUM_VERSION "6.4.0")
else()
    # 5.15 is the final Qt 5 LTS and the oldest release we support: it provides
    # QtNetworkAuth, versionless CMake commands and the string/container APIs
    # the codebase already relies on.
    set(MeshMC_QT_MINIMUM_VERSION "5.15.0")
endif()

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${MeshMC_QT_COMPONENTS})

if(DEFINED Qt${QT_VERSION_MAJOR}Core_VERSION)
    set(_projt_qt_version "${Qt${QT_VERSION_MAJOR}Core_VERSION}")
elseif(DEFINED Qt${QT_VERSION_MAJOR}_VERSION)
    set(_projt_qt_version "${Qt${QT_VERSION_MAJOR}_VERSION}")
endif()
if(DEFINED _projt_qt_version AND _projt_qt_version VERSION_LESS "${MeshMC_QT_MINIMUM_VERSION}")
    message(FATAL_ERROR "Qt ${MeshMC_QT_MINIMUM_VERSION} or newer is required. Detected: ${_projt_qt_version}")
endif()
message(STATUS "Building against Qt ${_projt_qt_version} (major version ${QT_VERSION_MAJOR})")
unset(_projt_qt_version)

if(UNIX)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_search_module(SCDOC scdoc)
        if(SCDOC_FOUND)
            pkg_get_variable(SCDOC_SCDOC scdoc scdoc)
            message(STATUS "SCDOC_FOUND=${SCDOC_FOUND}")
            message(STATUS "SCDOC_SCDOC=${SCDOC_SCDOC}")
        endif()
    endif()
endif()

include(QMakeQuery)
QUERY_QMAKE(QT_INSTALL_PLUGINS QT_PLUGINS_DIR)
QUERY_QMAKE(QT_INSTALL_LIBS QT_LIBS_DIR)
QUERY_QMAKE(QT_INSTALL_LIBEXECS QT_LIBEXECS_DIR)

# Map missing multi-config configurations to available ones.
# Monorepo libraries are built with single-config (Ninja), but MeshMC uses
# Ninja Multi-Config which validates all configurations at configure time.
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release Debug "")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL MinSizeRel Release Debug "")

find_package(cmark REQUIRED)

find_package(tomlplusplus 3.2.0)
if(NOT tomlplusplus_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(tomlplusplus REQUIRED IMPORTED_TARGET GLOBAL
                      tomlplusplus>=3.2.0)
    add_library(tomlplusplus::tomlplusplus ALIAS PkgConfig::tomlplusplus)
endif()

find_package(ZLIB REQUIRED)
find_package(LibArchive)
if(NOT LibArchive_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(libarchive REQUIRED IMPORTED_TARGET GLOBAL libarchive)
    add_library(LibArchive::LibArchive ALIAS PkgConfig::libarchive)
elseif(WIN32)
    # On Windows libarchive calls BCrypt* unconditionally (archive_util.c's
    # temp file helper, plus the CNG crypto backend vcpkg enables there), so a
    # static libarchive hands those symbols to whoever links it. FindLibArchive
    # reports nothing but the library file itself and vcpkg's wrapper only adds
    # the compression libraries, so without this every consumer fails to link
    # with undefined BCrypt* symbols.
    # ref. https://github.com/libarchive/libarchive/issues/3375
    #
    # Done once here rather than per target: the launcher, classparser and the
    # updater all pick the dependency up through this imported target. bcrypt
    # is a system import library, so it is harmless when libarchive is shared
    # and nothing actually references those symbols.
    set_property(TARGET LibArchive::LibArchive APPEND PROPERTY INTERFACE_LINK_LIBRARIES bcrypt)
endif()

if(MSVC)
    set(_meshmc_default_plugin_signatures OFF)
else()
    set(_meshmc_default_plugin_signatures ON)
endif()
option(MeshMC_PLUGIN_SIGNATURES
    "Enable GPG-backed plugin signature verification (requires Gpgmepp)"
    ${_meshmc_default_plugin_signatures})

if(MeshMC_PLUGIN_SIGNATURES)
    find_package(Gpgmepp REQUIRED)
    if(TARGET Gpgmepp)
        set(MeshMC_GpgmeppTarget Gpgmepp)
    elseif(TARGET GpgmeppStatic)
        set(MeshMC_GpgmeppTarget GpgmeppStatic)
    else()
        message(FATAL_ERROR
            "find_package(Gpgmepp) succeeded but exposed neither a Gpgmepp nor "
            "a GpgmeppStatic target, so there is nothing to link the plugin "
            "signature verifier against.\n"
            "Configure with -DMeshMC_PLUGIN_SIGNATURES=OFF to build the stub "
            "verifier instead.")
    endif()
    message(STATUS "Gpgmepp target: ${MeshMC_GpgmeppTarget}")

    get_target_property(_gpgmepp_loc ${MeshMC_GpgmeppTarget}
                        IMPORTED_LOCATION_NOCONFIG)
    get_target_property(_gpgmepp_son ${MeshMC_GpgmeppTarget}
                        IMPORTED_SONAME_NOCONFIG)
    if(_gpgmepp_loc)
        foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
            set_property(TARGET ${MeshMC_GpgmeppTarget} APPEND PROPERTY
                IMPORTED_CONFIGURATIONS ${_cfg})
            set_target_properties(${MeshMC_GpgmeppTarget} PROPERTIES
                IMPORTED_LOCATION_${_cfg} "${_gpgmepp_loc}")
            if(_gpgmepp_son)
                set_target_properties(${MeshMC_GpgmeppTarget} PROPERTIES
                    IMPORTED_SONAME_${_cfg} "${_gpgmepp_son}")
            endif()
        endforeach()
    endif()
    unset(_gpgmepp_loc)
    unset(_gpgmepp_son)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

####################################### Branding #######################################

######## Firstly, dont touch here, this section calculate Git commit, branch; Timestamp
######## and initialize targets for print version sections.

#### Check the current Git commit and branch
include(GetGitRevisionDescription)
get_git_head_revision(MeshMC_GIT_REFSPEC MeshMC_GIT_COMMIT ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR)
git_get_exact_tag(MeshMC_GIT_TAG)

message(STATUS "Git commit: ${MeshMC_GIT_COMMIT}")
message(STATUS "Git refspec: ${MeshMC_GIT_REFSPEC}")
message(STATUS "Git tag: ${MeshMC_GIT_TAG}")

string(TIMESTAMP TODAY "%Y-%m-%d")
set(MeshMC_BUILD_TIMESTAMP "${TODAY}")

#### Custom target to just print the version.
add_custom_target(version echo "Version: ${MeshMC_RELEASE_VERSION_NAME}")
add_custom_target(tcversion echo "\\#\\#teamcity[setParameter name=\\'env.MESHMC_VERSION\\' value=\\'${MeshMC_RELEASE_VERSION_NAME}\\']")

######## Secondly, please set version numbers ########
set(MeshMC_VERSION_MAJOR    11)
set(MeshMC_VERSION_MINOR    0)
set(MeshMC_VERSION_HOTFIX   0)

set(MeshMC_RELEASE_VERSION_NAME "${MeshMC_VERSION_MAJOR}.${MeshMC_VERSION_MINOR}.${MeshMC_VERSION_HOTFIX}")
set(MeshMC_RELEASE_TWO_NAME "${MeshMC_VERSION_MAJOR}.${MeshMC_VERSION_MINOR}")
set(MeshMC_VERSION_NAME4 "${MeshMC_VERSION_MAJOR}.${MeshMC_VERSION_MINOR}.${MeshMC_VERSION_HOTFIX}.0")
set(MeshMC_VERSION_NAME4_COMMA "${MeshMC_VERSION_MAJOR},${MeshMC_VERSION_MINOR},${MeshMC_VERSION_HOTFIX},0")

######## Thirdly, please define URLs and API keys ########

set(MeshMC_NEWS_RSS_URL "https://projecttick.org/feed/feed.xml" CACHE STRING "URL to fetch MeshMC's news RSS feed from.")
set(MeshMC_NEWS_EXTRA_FEEDS "https://projecttick.org/feed/feed.xml;" CACHE STRING "Semicolon-separated list of extra RSS feed URLs shown alongside the launcher's own news feed.")

# Build number
set(MeshMC_VERSION_BUILD -1 CACHE STRING "Build number. -1 for no build number.")

# Legacy substring artifact name (e.g. "MeshMC-Linux-Portable"). Used as a
# last-resort fallback when the product feed's assets do not carry the new
# structured attributes. Prefer setting the four structured cache vars below
# instead.
set(MeshMC_BUILD_ARTIFACT "" CACHE STRING "Legacy substring used to match a feed asset when structured attributes are unavailable.")

# Structured build identity. Matched directly against the feed asset's
# `platform`, `arch`, `portable` and `kind` attributes.
set(MeshMC_BUILD_PLATFORM_ID "" CACHE STRING "Updater asset platform id: linux | windows | macos.")
set(MeshMC_BUILD_ARCH        "" CACHE STRING "Updater asset arch: x86_64 | aarch64.")
set(MeshMC_BUILD_PORTABLE    "" CACHE STRING "Updater asset portable flag: true | false.")
set(MeshMC_BUILD_KIND        "" CACHE STRING "Updater asset kind: archive | appimage | installer.")

# Build platform.
set(MeshMC_BUILD_PLATFORM "" CACHE STRING "A short string identifying the platform that this build was built for. Only used by the notification system and to display in the about dialog.")

# Channel list URL (legacy - kept for compatibility)
set(MeshMC_UPDATER_BASE "https://projecttick.org/feed/feed.xml" CACHE STRING "Base URL for the legacy GoUpdate system (unused).")

# Updater: authoritative product feed URL (projt: namespace).
set(MeshMC_UPDATER_FEED_URL "https://projecttick.org/feed/feed.xml" CACHE STRING "RSS product feed URL for the updater (projt: namespace).")

# Updater: latest.json mirror URL (used as a cross-check; empty disables it).
set(MeshMC_UPDATER_LATEST_JSON_URL "https://ftp.projecttick.org/Project-Tick/latest.json" CACHE STRING "Project Tick latest.json mirror used as a sanity check against the feed.")

# Updater: the release channel this build subscribes to.
#
# A "stable" build is only ever offered <projt:channel>stable</projt:channel>
# feed entries; a "beta" build is offered beta *and* stable ones, so a beta
# user never gets stranded on an older version than the stable line.
#
# This is deliberately NOT MeshMC_VERSION_CHANNEL / BuildConfig.VERSION_CHANNEL,
# which is just the git branch name and purely informational.
set(MeshMC_UPDATE_CHANNEL "stable" CACHE STRING "Release channel this build subscribes to: stable | beta.")
set_property(CACHE MeshMC_UPDATE_CHANNEL PROPERTY STRINGS stable beta)

string(TOLOWER "${MeshMC_UPDATE_CHANNEL}" MeshMC_UPDATE_CHANNEL_EFFECTIVE)
if(MeshMC_UPDATE_CHANNEL_EFFECTIVE STREQUAL "")
    # CMakePresets forwards this from the environment, and an unset environment
    # variable expands to an empty string. Fall back to the safe channel rather
    # than shipping a build that accepts everything.
    set(MeshMC_UPDATE_CHANNEL_EFFECTIVE "stable")
elseif(NOT MeshMC_UPDATE_CHANNEL_EFFECTIVE MATCHES "^(stable|beta)$")
    message(FATAL_ERROR
        "MeshMC_UPDATE_CHANNEL must be 'stable' or 'beta' (got '${MeshMC_UPDATE_CHANNEL}').")
endif()
message(STATUS "Update channel: ${MeshMC_UPDATE_CHANNEL_EFFECTIVE}")

# Notification URL
set(MeshMC_NOTIFICATION_URL "https://projecttick.org/" CACHE STRING "URL for checking for notifications.")

# The metadata server
set(MeshMC_META_URL "https://meta.projecttick.org/" CACHE STRING "URL to fetch MeshMC's meta files from.")

# Microsoft Client ID
set(MeshMC_MICROSOFT_CLIENT_ID "3035382c-8f73-493a-b579-d182905c2864")

# paste.ee API key
set(MeshMC_PASTE_EE_API_KEY "ay2miY3lWxUHEEzYucj5e4YShIuo1aaY43MfwsUAe" CACHE STRING "API key you can get from paste.ee when you register an account")

# Imgur API Client ID
set(MeshMC_IMGUR_CLIENT_ID "5b97b0713fba4a3" CACHE STRING "Client ID you can get from Imgur when you register an application")

# CurseForge API key
set(MeshMC_CURSEFORGE_API_KEY "$2a$10$S7KcKijbCj8mCHUQcn0tgOmtHg0kA8q9FI0niNJJ7knPq0INomzrG" CACHE STRING "API key for the CurseForge API (get one at https://console.curseforge.com/)")

# Bug tracker URL
set(MeshMC_BUG_TRACKER_URL "https://github.com/Project-Tick/MeshMC/issues" CACHE STRING "URL for the bug tracker.")

# Discord URL
set(MeshMC_DISCORD_URL "https://discord.gg/PMxPTqwEwj" CACHE STRING "URL for the Discord guild.")

# Subreddit URL
set(MeshMC_SUBREDDIT_URL "" CACHE STRING "URL for the subreddit.")

# Feature flags
set(MeshMC_UNLEASH_URL "" CACHE STRING "GitLab Unleash-compatible feature flags endpoint.")
set(MeshMC_UNLEASH_INSTANCE_ID "" CACHE STRING "Unleash instance id. Set this to enable runtime feature flags.")
set(MeshMC_UNLEASH_APP_NAME "" CACHE STRING "Unleash application name / environment (UNLEASH-APPNAME). Must match the flag's environment spec, e.g. 'production'.")

######## Fourthly, please define MeshMC attributes ########

set(MeshMC_CommonName "MeshMC")
set(MeshMC_AppBinaryName "meshmc")
set(MeshMC_EnvName "MESHMC")
set(MeshMC_AppID "org.projecttick.MeshMC")
set(MeshMC_Copyright "Project Tick")
set(MeshMC_Copyright_Mac "© 2026 Project Tick")
set(MeshMC_Domain "projecttick.org")
set(MeshMC_Name "${MeshMC_CommonName}")
set(MeshMC_DisplayName "${MeshMC_CommonName}")
set(MeshMC_UserAgent "${MeshMC_CommonName}/${MeshMC_RELEASE_TWO_NAME}")
set(MeshMC_ConfigFile "meshmc.cfg")
set(MeshMC_Git "https://github.com/Project-Tick/MeshMC")
set(MeshMC_Patreon_Url "https://www.patreon.com/meshmc")
set(MeshMC_Authors "Copyright (C) 2026 Project Tick <https://projecttick.org>, All rights reserved. This Project Licensed under GNU GENERAL PUBLIC LICENSE, either version 3 of the License, or any later version. ")

######## Lastly, configure placeholders

set(MeshMC_SVGFileName "${MeshMC_AppID}.svg")
set(MeshMC_Branding_ICNS "branding/${MeshMC_AppID}.icns")
set(MeshMC_Branding_ICO "${MeshMC_AppID}.ico")
set(MeshMC_Branding_WindowsRC "branding/${MeshMC_AppBinaryName}.rc")
set(MeshMC_Branding_LogoQRC "branding/${MeshMC_AppBinaryName}.qrc")

configure_file(branding/${MeshMC_AppID}.desktop.in ${MeshMC_AppID}.desktop)
configure_file(branding/${MeshMC_AppID}.metainfo.xml.in ${MeshMC_AppID}.metainfo.xml)
configure_file(branding/${MeshMC_AppBinaryName}.rc.in ${MeshMC_AppBinaryName}.rc @ONLY)
configure_file(branding/${MeshMC_AppBinaryName}.qrc.in ${MeshMC_AppBinaryName}.qrc @ONLY)
configure_file(branding/${MeshMC_CommonName}.manifest.in ${MeshMC_AppBinaryName}.manifest @ONLY)
configure_file(branding/${MeshMC_AppID}.mime.xml ${MeshMC_AppID}.xml COPYONLY)
configure_file(branding/${MeshMC_SVGFileName} ${MeshMC_SVGFileName} COPYONLY)
configure_file(branding/${MeshMC_AppID}.ico ${MeshMC_AppID}.ico COPYONLY)
configure_file(branding/${MeshMC_AppBinaryName}.6.scd.in ${MeshMC_AppBinaryName}.6.scd @ONLY)
set(MeshMC_Portable_File "branding/portable.txt")
set(MeshMC_Branding_MAC_ICON "branding/${MeshMC_CommonName}.icon")

if(SCDOC_FOUND)
    set(in_scd "${CMAKE_CURRENT_BINARY_DIR}/${MeshMC_AppBinaryName}.6.scd")
    set(out_man "${CMAKE_CURRENT_BINARY_DIR}/${MeshMC_AppBinaryName}.6")
    add_custom_command(
        DEPENDS "${in_scd}"
        OUTPUT "${out_man}"
        COMMAND ${SCDOC_SCDOC} < "${in_scd}" > "${out_man}"
    )
    add_custom_target(man ALL DEPENDS ${out_man})
    set(MeshMC_ManPage "${CMAKE_CURRENT_BINARY_DIR}/${MeshMC_AppBinaryName}.6")
endif()

if(MSVC)
    set(MeshMC_MSVC_Redist_NSIS_Section [=[
!ifdef haveNScurl
Section "Visual Studio Runtime"
    Var /GLOBAL vc_redist_exe
    ${If} ${IsNativeARM64}
        StrCpy $vc_redist_exe "vc_redist.arm64.exe"
    ${Else}
        StrCpy $vc_redist_exe "vc_redist.x64.exe"
    ${EndIf}
    DetailPrint 'Downloading Microsoft Visual C++ Redistributable...'
    NScurl::http GET "https://aka.ms/vs/17/release/$vc_redist_exe" "$INSTDIR\vc_redist\$vc_redist_exe" /INSIST /CANCEL /Zone.Identifier /END
    Pop $0
    ${If} $0 == "OK"
        DetailPrint "Download successful"
        ExecWait "$INSTDIR\vc_redist\$vc_redist_exe /install /passive /norestart"
    ${Else}
        DetailPrint "Download failed with error $0"
    ${EndIf}
SectionEnd
!endif
]=])
endif()

configure_file(branding/win_install.nsi.in win_install.nsi @ONLY)

####################################### Install layout #######################################

if(NOT (UNIX AND APPLE))
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${MeshMC_Portable_File}" DESTINATION "." COMPONENT portable EXCLUDE_FROM_ALL)
endif()

if(UNIX AND APPLE)
    set(BINARY_DEST_DIR "${MeshMC_Name}.app/Contents/MacOS")
    set(FRAMEWORK_DEST_DIR "${MeshMC_Name}.app/Contents/Frameworks")
    set(LIBRARY_DEST_DIR "${FRAMEWORK_DEST_DIR}")
    set(PLUGIN_DEST_DIR "${MeshMC_Name}.app/Contents/MacOS")
    set(RESOURCES_DEST_DIR "${MeshMC_Name}.app/Contents/Resources")
    set(JARS_DEST_DIR "${MeshMC_Name}.app/Contents/Resources/jars")
    set(BUNDLE_DEST_DIR ".")
    set(MMCO_MODULES_DEST_DIR
        "${MeshMC_Name}.app/Contents/Resources/mmcmodules")
    set(MMCO_PLUGIN_DATA_DEST_DIR
        "${MeshMC_Name}.app/Contents/Resources/MMCOPluginData")

    # Mac bundle settings
    set(MACOSX_BUNDLE_BUNDLE_NAME "${MeshMC_DisplayName}")
    set(MACOSX_BUNDLE_INFO_STRING "${MeshMC_DisplayName}: A custom launcher for Minecraft that allows you to easily manage multiple installations of Minecraft at once.")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "${MeshMC_AppID}")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${MeshMC_RELEASE_VERSION_NAME}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${MeshMC_RELEASE_VERSION_NAME}")
    set(MACOSX_BUNDLE_LONG_VERSION_STRING "${MeshMC_RELEASE_VERSION_NAME}")
    set(MACOSX_BUNDLE_ICON_FILE ${MeshMC_AppID}.icns)
    set(MACOSX_BUNDLE_COPYRIGHT "${MeshMC_Copyright_Mac}")
    set(MACOSX_SPARKLE_UPDATE_PUBLIC_KEY "C0eBoyDSoZbzgCMxQH9wH6kmjU2mPRmvhZZd9mHgqZQ=" CACHE STRING "Public key for Sparkle update feed")
    set(MACOSX_SPARKLE_UPDATE_FEED_URL "https://projecttick.org/feed/appcast.xml" CACHE STRING "URL for Sparkle update feed")

    set(MACOSX_SPARKLE_DOWNLOAD_URL "https://github.com/sparkle-project/Sparkle/releases/download/2.8.0/Sparkle-2.8.0.tar.xz" CACHE STRING "URL to Sparkle release archive")
    set(MACOSX_SPARKLE_SHA256 "fd5681ee92bf238aaac2d08214ceaf0cc8976e452d7f882d80bac1e61581f3b1" CACHE STRING "SHA256 checksum for Sparkle release archive")
    set(MACOSX_SPARKLE_DIR "${CMAKE_BINARY_DIR}/frameworks/Sparkle")

    # Add the icon
    install(FILES ${MeshMC_Branding_ICNS} DESTINATION ${RESOURCES_DEST_DIR} RENAME ${MeshMC_AppID}.icns)

    find_program(ACTOOL_EXE actool DOC "Path to the apple asset catalog compiler")
    if(ACTOOL_EXE)
        execute_process(
            COMMAND xcodebuild -version
            OUTPUT_VARIABLE XCODE_VERSION_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        string(REGEX MATCH "Xcode ([0-9]+\.[0-9]+)" XCODE_VERSION_MATCH "${XCODE_VERSION_OUTPUT}")
        if(XCODE_VERSION_MATCH)
            set(XCODE_VERSION ${CMAKE_MATCH_1})
        else()
            set(XCODE_VERSION 0.0)
        endif()

        if(XCODE_VERSION VERSION_GREATER_EQUAL 26.0)
            set(ASSETS_OUT_DIR "${CMAKE_BINARY_DIR}/branding")
            set(GENERATED_ASSETS_CAR "${ASSETS_OUT_DIR}/Assets.car")
            set(ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${MeshMC_Branding_MAC_ICON}")

            add_custom_command(
                OUTPUT "${GENERATED_ASSETS_CAR}"
                COMMAND ${ACTOOL_EXE} "${ICON_SOURCE}"
                        --compile "${ASSETS_OUT_DIR}"
                        --output-partial-info-plist /dev/null
                        --app-icon ${MeshMC_Name}
                        --enable-on-demand-resources NO
                        --target-device mac
                        --minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET}
                        --platform macosx
                DEPENDS "${ICON_SOURCE}"
                COMMENT "Compiling asset catalog (${ICON_SOURCE})"
                VERBATIM
            )
            add_custom_target(compile_assets ALL DEPENDS "${GENERATED_ASSETS_CAR}")
            install(FILES "${GENERATED_ASSETS_CAR}" DESTINATION "${RESOURCES_DEST_DIR}")
        else()
            message(WARNING "Xcode ${XCODE_VERSION} is too old. Minimum required version is 26.0. Not compiling liquid glass icons.")
        endif()

    else()
        message(WARNING "actool not found. Cannot compile macOS app icons.\n"
                "Install Xcode command line tools: 'xcode-select --install'")
    endif()


elseif(UNIX)
    include(GNUInstallDirs)

    set(BINARY_DEST_DIR "${CMAKE_INSTALL_BINDIR}")
    set(LIBRARY_DEST_DIR "${CMAKE_INSTALL_LIBDIR}")
    set(JARS_DEST_DIR "share/${MeshMC_Name}")
    set(MMCO_MODULES_DEST_DIR "share/${MeshMC_Name}/mmcmodules")
    set(MMCO_PLUGIN_DATA_DEST_DIR "${MMCO_MODULES_DEST_DIR}")
    set(PRIVATE_LIBRARY_DEST_DIR "${CMAKE_INSTALL_LIBDIR}/${MeshMC_AppBinaryName}")
    SET(MeshMC_BINARY_RPATH "$ORIGIN/../${PRIVATE_LIBRARY_DEST_DIR}")
    set(KDE_INSTALL_APPDIR      "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    set(KDE_INSTALL_METAINFODIR "${CMAKE_INSTALL_DATAROOTDIR}/metainfo")
    set(KDE_INSTALL_ICONDIR     "${CMAKE_INSTALL_DATAROOTDIR}/icons")
    set(KDE_INSTALL_MIMEDIR     "${CMAKE_INSTALL_DATAROOTDIR}/mime/packages")
    set(KDE_INSTALL_MANDIR      "${CMAKE_INSTALL_MANDIR}")

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${MeshMC_AppID}.desktop DESTINATION ${KDE_INSTALL_APPDIR})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${MeshMC_AppID}.metainfo.xml DESTINATION ${KDE_INSTALL_METAINFODIR})
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/branding/${MeshMC_AppID}.svg DESTINATION "${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps")
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/branding/${MeshMC_AppID}_256.png DESTINATION "${KDE_INSTALL_ICONDIR}/hicolor/256x256/apps" RENAME "${MeshMC_AppID}.png")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${MeshMC_AppID}.xml DESTINATION ${KDE_INSTALL_MIMEDIR})

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/launcher/qtlogging.ini" DESTINATION "share/${MeshMC_Name}")

    set(PLUGIN_DEST_DIR "plugins")
    set(BUNDLE_DEST_DIR ".")
    set(RESOURCES_DEST_DIR ".")

    if(MeshMC_ManPage)
        install(FILES ${MeshMC_ManPage} DESTINATION "${KDE_INSTALL_MANDIR}/man6")
    endif()

    # Install basic runner script if component "portable" is selected
    configure_file(launcher/MeshMC.in "${CMAKE_CURRENT_BINARY_DIR}/MeshMCScript" @ONLY)
    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/MeshMCScript" DESTINATION "." RENAME ${MeshMC_Name} COMPONENT portable EXCLUDE_FROM_ALL)

elseif(WIN32)
    set(BINARY_DEST_DIR ".")
    set(LIBRARY_DEST_DIR ".")
    set(PLUGIN_DEST_DIR ".")
    set(FRAMEWORK_DEST_DIR ".")
    set(RESOURCES_DEST_DIR ".")
    set(JARS_DEST_DIR "jars")
    # Windows keeps the flat layout it has always used: .mmco files
    # next to meshmc.exe under <install>/mmcmodules. Variable exists
    # so plugin CMakeLists files can refer to it uniformly.
    set(MMCO_MODULES_DEST_DIR "${BINARY_DEST_DIR}/mmcmodules")
    # Windows has no codesign-style strict mode that distinguishes
    # code from data inside the plugin directory, so plugin data
    # can stay alongside the .mmco. Variable mirrors the macOS one
    # for cross-platform plugin CMakeLists.
    set(MMCO_PLUGIN_DATA_DEST_DIR "${MMCO_MODULES_DEST_DIR}")
else()
    message(FATAL_ERROR "Platform not supported")
endif()

################################ Included Libs ################################

include(ExternalProject)
set_directory_properties(PROPERTIES EP_BASE External)

# Everything under libraries/ is built as part of this build tree, statically,
# unless the library hardcodes SHARED (iconfix, rainbow) or opts out itself
# (nbt++, via NBT_BUILD_SHARED). This matches what the old pre-built
# dependency prefix shipped, so linkage does not change — only where the
# objects come from.
#
# Cache entry rather than a plain variable: subprojects under libraries/ read
# this through option(), and a cache entry is the one form that wins there
# whichever CMP0077 mode their cmake_minimum_required happens to select.

############################### internal libraries ############################
option(NBT_BUILD_SHARED "Build NBT shared library" ON)
option(NBT_USE_ZLIB "Build NBT library with zlib support" OFF)
option(NBT_BUILD_TESTS "Build NBT library tests" OFF) #FIXME: fix unit tests.

add_subdirectory(${CMAKE_SOURCE_DIR}/libraries/libnbtplusplus libnbtplusplus)

add_subdirectory(libraries/launcher) # java based launcher part for Minecraft
add_subdirectory(libraries/javacheck) # java compatibility checker
add_subdirectory(libraries/xz-embedded) # xz compression
add_subdirectory(libraries/rainbow) # Qt extension for colors
add_subdirectory(libraries/iconfix) # fork of Qt's QIcon loader
add_subdirectory(libraries/localpeer) # fork of a library from Qt solutions
add_subdirectory(libraries/classparser) # Java class file parser
add_subdirectory(libraries/katabasis) # An OAuth2 library that tried to do too much

# Three libraries under libraries/ build as shared objects (iconfix and rainbow
# hardcode SHARED, nbt++ opts in via NBT_BUILD_SHARED), so they have to be
# deployed where the launcher can load them: alongside the binary on Windows,
# LIBRARY_DEST_DIR elsewhere (covered by the RPATH, $ORIGIN/../lib).
#
# Installed explicitly rather than left to install(RUNTIME_DEPENDENCY_SET),
# which scans the built binary and searches a directory list — it did not pick
# these three up. Their own subprojects do install them, but to
# ${CMAKE_INSTALL_BINDIR} (install/bin/) while meshmc.exe sits at the install
# root, so the launcher died on startup with "nbt++.dll not found". These are
# our own targets and we know exactly where they belong; no point rediscovering
# it by scanning a binary.
set_target_properties(nbt++ iconfix rainbow PROPERTIES
    VERSION   "${MeshMC_RELEASE_VERSION_NAME}"
    SOVERSION "${MeshMC_VERSION_MAJOR}")

if(NOT DEFINED PRIVATE_LIBRARY_DEST_DIR)
    set(PRIVATE_LIBRARY_DEST_DIR "${LIBRARY_DEST_DIR}")
endif()

install(TARGETS nbt++ iconfix rainbow
    RUNTIME DESTINATION "${BINARY_DEST_DIR}"
    LIBRARY DESTINATION "${PRIVATE_LIBRARY_DEST_DIR}")

# The Java helpers are built in-tree now, so their jars come out of the build
# directory. The launcher looks for them by exact name under JARS_DEST_DIR
# (see Application::getJarsPath()), which is not where install_jar() puts them.
get_target_property(JAVACHECK_JAR JavaCheck JAR_FILE)
get_target_property(NEWLAUNCH_JAR NewLaunch JAR_FILE)
install(FILES "${JAVACHECK_JAR}" DESTINATION "${JARS_DEST_DIR}" RENAME "JavaCheck.jar")
install(FILES "${NEWLAUNCH_JAR}" DESTINATION "${JARS_DEST_DIR}" RENAME "NewLaunch.jar")

############################### Built Artifacts ###############################

add_subdirectory(buildconfig)

# The standalone meshmc-updater binary is built whenever the updater feed URL
# is configured. It takes the artifact URL on the command line (--url), so it
# needs no build identity of its own -- gating it behind one only meant that
# developer builds silently shipped without an updater and that the binary
# could rot unnoticed between releases.
#
# Whether the *launcher* offers updates is a separate decision, made by
# BuildConfig.UPDATER_ENABLED (see buildconfig/BuildConfig.cpp.in).
if(NOT MeshMC_UPDATER_FEED_URL STREQUAL "")
    add_subdirectory(launcher/updater/MeshMCUpdater)
endif()

add_subdirectory(launcher/crashreporter)

# NOTE: this must always be last to appease the CMake deity of quirky install command evaluation order.
add_subdirectory(launcher)

if(MeshMC_PLUGINS)
    # Plugin SDK (INTERFACE library — headers only, must come before plugins)
    add_subdirectory(launcher/plugin/sdk)

    # In-tree .mmco plugins (after launcher so MeshMC_logic target is available)
    add_subdirectory(launcher/plugin/plugins)
endif()
