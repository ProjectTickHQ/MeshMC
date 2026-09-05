# SPDX-FileCopyrightText: 2026 Project Tick
# SPDX-FileContributor: Project Tick
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

project(meshmc-updater)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Widgets buys exactly one thing: the ability to tell the user that an update
# failed. Without a console (see the WIN32 flag below) a silent failure looks
# identical to success, which is how this binary stayed broken unnoticed. The
# stage that does the overwriting runs from the staging directory, so the extra
# Qt libraries are never mapped out of the installation being replaced and cost
# nothing in lock contention.
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network Widgets)

# Normally already resolved by the top-level build (from vcpkg, or from the
# system package); looked up here too so this directory still configures alone.
if(NOT TARGET LibArchive::LibArchive)
    find_package(LibArchive REQUIRED)
endif()

# On Windows: use Win32 subsystem so no console window appears. Diagnostics go
# to the log file instead (UpdaterLog), and --verbose adopts the console of
# whoever started us.
if(WIN32)
    set(UPDATER_WIN32_FLAG WIN32)
else()
    set(UPDATER_WIN32_FLAG "")
endif()

add_executable(meshmc-updater ${UPDATER_WIN32_FLAG}
    main.cpp
    UpdaterOptions.h
    UpdaterOptions.cpp
    UpdaterLog.h
    UpdaterLog.cpp
    UpdaterUtil.h
    UpdaterUtil.cpp
    UpdateLock.h
    UpdateLock.cpp
    ArchiveReader.h
    ArchiveReader.cpp
    PrepareStage.h
    PrepareStage.cpp
    ApplyStage.h
    ApplyStage.cpp
)

target_link_libraries(meshmc-updater
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Widgets
        LibArchive::LibArchive
        BuildConfig
)

if(APPLE)
    set_target_properties(meshmc-updater PROPERTIES
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
endif()

# UnitTest.cmake is pulled in here rather than inherited, because this
# directory is added before launcher/, which is where the launcher's own
# include(UnitTest) lives.
include(UnitTest)

# UpdaterOptions is the only piece of the updater that is pure logic, and it is
# also where the "updater does nothing at all" bug lived: a shell that does not
# strip quotes. It compiles against Qt Core alone, so the test just builds
# the source again rather than forcing the updater into a static library.
add_unit_test(UpdaterOptions
    SOURCES UpdaterOptions_test.cpp UpdaterOptions_test.h UpdaterOptions.cpp UpdaterOptions.h
    LIBS Qt${QT_VERSION_MAJOR}::Core
    )

# The lock file doubles as the hand-off signal between the two stages: the
# second stage rewrites it under its own process id, and that is the only way
# the first stage can tell "I started it" apart from "it actually ran". Getting
# that wrong made a failed update look exactly like a successful one, so it is
# covered here instead of being left to a manual run.
add_unit_test(UpdateLock
    SOURCES UpdateLock_test.cpp UpdateLock_test.h UpdateLock.cpp UpdateLock.h UpdaterUtil.cpp UpdaterUtil.h
    LIBS Qt${QT_VERSION_MAJOR}::Core
    )

# Install alongside the main binary so UpdateController can find it.
install(TARGETS meshmc-updater
    RUNTIME DESTINATION "${BINARY_DEST_DIR}"
)
