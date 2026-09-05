# MeshMC Build Guide

This document explains how to build MeshMC from source on all supported platforms.

## Table of Contents

- [Requirements](#requirements)
- [Dependencies](#dependencies)
- [Cloning the Repository](#cloning-the-repository)
- [CMake Presets](#cmake-presets)
- [Choosing the Qt version](#choosing-the-qt-version)
- [Building on Linux](#building-on-linux)
- [Building on macOS](#building-on-macos)
- [Building on Windows](#building-on-windows)
- [Building with Nix](#building-with-nix)
- [Building with Container (Podman/Docker)](#building-with-container-podmandocker)
- [Running Tests](#running-tests)
- [CMake Options](#cmake-options)
- [Troubleshooting](#troubleshooting)

## Requirements

- **CMake** >= 3.20
- **Ninja** (recommended generator)
- **C++ compiler** with C++23 support (GCC >= 13, Clang >= 17, MSVC >= 19.36)
- **Qt 6** (>= 6.4) *or* **Qt 5** (>= 5.15) — Core, Widgets, Concurrent,
  Network, NetworkAuth, Test, Xml, OpenGL. Qt 6 is the default; see
  [Choosing the Qt version](#choosing-the-qt-version).
- **Java Development Kit** (JDK 17) — for building Java launcher components
- **Git** — required at configure time: vcpkg is a submodule, and it fetches
  the port versions it resolves through git

Everything else (libarchive, cmark, toml++, zlib, libqrencode, ECM) is installed
by vcpkg when you configure with a preset — see
[Dependencies](#dependencies).

## Dependencies

### Build Dependencies

| Dependency             | Purpose                         | pkg-config name    |
|------------------------|---------------------------------|--------------------|
| Qt Base (6 or 5)       | GUI framework                   | `Qt6Core` / `Qt5Core` |
| Qt NetworkAuth         | OAuth2 authentication           | —                  |
| Extra CMake Modules    | KDE CMake utilities             | `ECM`              |
| libqrencode            | QR code generation              | —                  |
| scdoc                  | Man page generation (optional)  | —                  |

### Dependencies Managed by vcpkg

vcpkg is vendored as a submodule at `cmake/vcpkg`, and the CMake presets point
their toolchain file at it. Configuring with a preset therefore installs
everything listed in `vcpkg.json` first, into `build/vcpkg_installed/`:

| Dependency   | Notes                                                    |
|--------------|----------------------------------------------------------|
| ECM          | host dependency — CMake modules only, nothing is linked   |
| cmark        | Markdown rendering                                        |
| gpgmepp      | plugin signature verification — **not on MSVC**, see below |
| libarchive   | features: `bzip2`, `lz4`, `lzma`, `zstd`                  |
| libqrencode  | optional; the QR code in the crash reporter               |
| tomlplusplus | TOML parsing                                              |
| zlib         | the plain zlib API, for nbt++ and the launcher            |

`gpgmepp` (which pulls in `gpgme`, `libassuan` and `libgpg-error`) carries the
platform expression `!windows`, so on Linux and macOS it comes from vcpkg like
everything else. Windows is split:

- **MinGW** takes it from MSYS2 instead, through `gpgme:p` in the CI's pacboy
  list. That package is gpgme 1.23.x, i.e. from before the 2.0 split, so it
  still bundles the C++ binding — `include/gpgme++`, `lib/cmake/Gpgmepp` and
  `libgpgmepp` are all in it, and `find_package(Gpgmepp)` finds them with no
  help. Building the same chain through vcpkg is not merely slower there:
  gpgme's `make all` fails under clang64.
- **MSVC** gets nothing, because upstream gpgme is autotools and does not
  support MSVC — the vcpkg port declares as much. This is the only platform
  where `MeshMC_PLUGIN_SIGNATURES` defaults to `OFF` and the verifier compiles
  into a stub.

Only the C++ binding is used (`gpgme++/` headers, the `GpgME::` namespace);
QGpgME is not needed, and has no vcpkg port anyway.

Those three are also the only autotools packages in the dependency set, which
is why `cmake/vcpkg-triplets/universal-osx.cmake` sets `VCPKG_MAKE_BUILD_TRIPLET`:
vcpkg would otherwise configure them with `--host=universal-apple-darwin`, a
name `config.sub` rejects. The triplet pins a real GNU triplet instead, matching
the build machine, and the universal binary keeps coming from the
`-arch arm64 -arch x86_64` flags vcpkg already passes to the compiler.

Note that with the static triplets used here, gpgmepp exports its target as
`GpgmeppStatic` rather than `Gpgmepp`; the top-level `CMakeLists.txt` accepts
either.

Two files drive this:

- **`vcpkg.json`** — the dependency list and the features asked of each one.
- **`vcpkg-configuration.json`** — pins the registry to a single upstream vcpkg
  commit (`baseline`), so everyone resolves the same versions, and points at two
  overlay directories:
  - `cmake/vcpkg-triplets/` — static libraries, dynamic CRT, release-only
    dependencies. The README there explains why, including the autotools
    `--host` pin the universal macOS triplet needs.
  - `cmake/vcpkg-ports/` — currently one port, `vcpkg-tool-meson`: upstream's,
    plus a patch that stops meson's compiler detection from choking on
    `-arch arm64 -arch x86_64`. Without it `tomlplusplus` (the one meson-built
    dependency) cannot configure for `universal-osx`. Again, see the README in
    that directory.

Qt is deliberately **not** in the manifest. It stays a system dependency,
because nobody wants to build Qt from source.

The first configure of a fresh build tree needs network access, and takes a
while: vcpkg builds each dependency from source once, then reuses its binary
cache (`~/.cache/vcpkg` on Linux/macOS) for every later build tree.

**Building without vcpkg is supported**, and is what distro packaging does:
configure without the presets (or override `CMAKE_TOOLCHAIN_FILE`) and the same
`find_package()` calls resolve against system packages instead — with
`pkg-config` as an additional fallback for libarchive and toml++, which distros
often ship without CMake package files. Install the libraries in the table above
through your package manager in that case.

### In-Tree Dependencies

nbt++, iconfix, rainbow, classparser and the other libraries under
`libraries/` are git subtrees, built as part of this tree. These are **not**
system packages — do not install distro versions of them.

### Distro-Specific Package Names

<details>
<summary><strong>Debian / Ubuntu</strong></summary>

```bash
sudo apt-get install \
    cmake ninja-build extra-cmake-modules pkg-config \
    qt6-base-dev libqrencode-dev \
    scdoc
```

</details>

<details>
<summary><strong>Fedora</strong></summary>

```bash
sudo dnf install \
    cmake ninja-build extra-cmake-modules pkgconf \
    qt6-qtbase-devel qrencode-devel \
    gpgmepp-devel \
    scdoc
```

`gpgmepp-devel` is only needed for the non-vcpkg path (it is what
`MeshMC_PLUGIN_SIGNATURES` links against); `qgpgme-qt6-devel` is **not**
required — the code never uses QGpgME.

</details>

<details>
<summary><strong>Arch Linux</strong></summary>

```bash
sudo pacman -S --needed \
    cmake ninja extra-cmake-modules pkgconf \
    qt6-base qrencode \
    scdoc
```

</details>

<details>
<summary><strong>openSUSE</strong></summary>

```bash
sudo zypper install \
    cmake ninja extra-cmake-modules pkg-config \
    qt6-base-devel qrencode-devel \
    scdoc
```

</details>

<details>
<summary><strong>macOS (Homebrew)</strong></summary>

```bash
brew install \
    cmake ninja extra-cmake-modules \
    qt@6 qrencode \
    scdoc
```

</details>

### Developer Tooling (Optional)

These are **not required** to build, but are used for development:

| Tool       | Purpose                       |
|------------|-------------------------------|
| npm        | Frontend tooling              |
| Go         | Installing lefthook           |
| lefthook   | Git hooks manager             |
| reuse       | REUSE license compliance      |
| clang-format | Code formatting              |
| clang-tidy | Static analysis               |

## Cloning the Repository

MeshMC uses one git submodule — vcpkg, at `cmake/vcpkg`. It is the CMake
toolchain the presets use, so a non-recursive clone cannot configure. Clone
recursively:

```bash
git clone --recursive https://github.com/Project-Tick/Project-Tick.git
cd Project-Tick/MeshMC
```

If you already cloned without `--recursive`, initialize submodules manually:

```bash
git submodule update --init --recursive
```

## Building Dependencies

You can use to build MeshMC dependencies for git repository, please use build-deps script.

Linux / macOS / Windows MinGW

```bash

./scripts/build-deps.sh

```

Windows MSVC

```pwsh

.\scripts\build-deps.ps1

```

## CMake Presets

MeshMC ships a `CMakePresets.json` with pre-configured presets for each platform.
All presets use the **Ninja Multi-Config** generator and output to the `build/`
directory with install prefix `install/`.

### Configure Presets

| Preset             | Platform                  | Notes                                      |
|--------------------|---------------------------|--------------------------------------------|
| `linux`            | Linux                     | Available only on Linux hosts               |
| `macos`            | macOS                     | Available only on macOS hosts               |
| `macos_universal`  | macOS (Universal Binary)  | Builds for both x86_64 and arm64            |
| `windows_mingw`    | Windows (MinGW)           | Available only on Windows hosts             |
| `windows_msvc`     | Windows (MSVC)            | Available only on Windows hosts             |

All presets inherit from a hidden `base` preset which sets:

- **Generator:** `Ninja Multi-Config`
- **Build directory:** `build/`
- **Install directory:** `install/`
- **LTO:** Enabled by default

### Build Presets

Each configure preset has a matching build preset with the same name:

| Preset             | Configure Preset   |
|--------------------|--------------------|
| `linux`            | `linux`            |
| `macos`            | `macos`            |
| `macos_universal`  | `macos_universal`  |
| `windows_mingw`    | `windows_mingw`    |
| `windows_msvc`     | `windows_msvc`     |

### Test Presets

Test presets share the same names. They are configured with verbose output on
failure and exclude example tests.

### Environment Variables

Some presets reference environment variables:

| Variable          | Used By                        | Purpose                           |
|-------------------|--------------------------------|-----------------------------------|
| `ARTIFACT_NAME`   | All (via `base`)               | Updater artifact identifier       |
| `BUILD_PLATFORM`  | All (via `base`)               | Platform identifier string        |

## Choosing the Qt version

MeshMC builds against either Qt 6 (>= 6.4) or Qt 5 (>= 5.15). One cache
variable selects it, and it defaults to Qt 6:

```bash
# Qt 6 (default)
cmake --preset linux

# Qt 5
cmake --preset linux -DMeshMC_QT_VERSION_MAJOR=5
```

Configure prints which one it resolved, so it is worth a glance:

```
-- Building against Qt 5.15.18 (major version 5)
```

Notes:

- **Use a separate build directory per Qt version.** The major version is
  baked into cached CMake state and into every generated `ui_*.h`; switching
  it inside one build tree means reconfiguring from scratch anyway.
- **Qt 5 needs the same modules as Qt 6**, plus `NetworkAuth` (a separate
  package on most distros — e.g. `qt5-qtnetworkauth-devel`,
  `libqt5networkauth5-dev`).
- **No `OpenGLWidgets` package on Qt 5.** Qt 6 split `QOpenGLWidget` into
  its own module; on Qt 5 it lives in QtWidgets, and the build accounts for
  this. Nothing extra to install.
- **macOS bundles are Qt 6 only.** Bundle deployment uses
  `qt_generate_deploy_script`, which has no Qt 5 counterpart. Configuring a
  macOS bundle with Qt 5 fails at configure time with an explicit message
  rather than silently producing an incomplete `.app`.
- **Out-of-tree plugins must match the launcher.** The exported
  `MeshMC::SDK` target names `Qt5::*`/`Qt6::*` targets concretely. The
  installed SDK config reports what it was built against in
  `MeshMC_SDK_QT_VERSION_MAJOR`.

Qt APIs that genuinely differ between the two majors are shimmed in one
place, `launcher/QtCompat.h`, rather than with `#if`s spread across call
sites. Anything that compiles unchanged against both does not belong there.

## Building on Linux

### Configure

```bash
cmake --preset linux
```

### Build

```bash
cmake --build --preset linux --config Release
```

For a debug build:

```bash
cmake --build --preset linux --config Debug
```

Since the generator is `Ninja Multi-Config`, you can switch between `Debug`,
`Release`, `RelWithDebInfo`, and `MinSizeRel` without re-configuring.

### Install

```bash
cmake --install build --config Release --prefix /usr/local
```

### Full One-Liner

```bash
cmake --preset linux && cmake --build --preset linux --config Release
```

## Building on macOS

### Prerequisites

Install dependencies via Homebrew:

```bash
brew install cmake ninja extra-cmake-modules qt@6 qrencode pkg-config
```

libarchive is **not** installed from Homebrew: vcpkg provides it, along with
cmark, toml++, zlib and libqrencode — so there is no `CMAKE_PREFIX_PATH`
juggling for keg-only formulae. Do check the submodule out first, since vcpkg
itself is one:

```bash
git submodule update --init --recursive
```

### Standard Build (Native Architecture)

```bash
cmake --preset macos
cmake --build --preset macos --config Release
```

### Universal Binary (x86_64 + arm64)

```bash
cmake --preset macos_universal
cmake --build --preset macos_universal --config Release
```

This produces a fat binary supporting both Intel and Apple Silicon Macs.

### Install

```bash
cmake --install build --config Release
```

## Building on Windows

### Using MSVC

Requires Visual Studio with C++ workload.

No external dependency has to be installed by hand: ECM, libarchive, cmark,
toml++, zlib and libqrencode all come from vcpkg during configure, and the
libraries under `libraries/` are part of this repository. Just make sure the
submodule is checked out, since vcpkg is one:

```cmd
git submodule update --init --recursive
```

`pkg-config` is **not** needed for an MSVC build — libqrencode is looked up
directly there (`find_path` / `find_library`), precisely because MSVC has no
usable pkg-config.

Then configure and build:

```cmd
cmake --preset windows_msvc
cmake --build --preset windows_msvc --config Release
```

### Using MinGW

```cmd
cmake --preset windows_mingw
cmake --build --preset windows_mingw --config Release
```

### Install

```cmd
cmake --install build --config Release
```

## Building with Nix

MeshMC provides a Nix flake for reproducible builds.

### Using the Nix Flake

```bash
# Build the package
nix build .#meshmc

# Enter the development shell
nix develop

# Inside the dev shell:
cd "$cmakeBuildDir"
ninjaBuildPhase
ninjaInstallPhase
```

### Without Flakes

```bash
nix-build
# or
nix-shell
```

### Binary Cache

A binary cache is available to speed up builds:

```
https://meshmc.cachix.org
```

The public key is:

```
meshmc.cachix.org-1:6ZNLcfqjVDKmN9/XNWGV3kcjBTL51v1v2V+cvanMkZA=
```

These are already configured in the flake's `nixConfig`.

## Building with Container (Podman/Docker)

A `Containerfile` (Debian-based) is provided for CI and reproducible builds.

### Build the Container Image

```bash
podman build -t meshmc-build .
```

### Run a Build Inside the Container

```bash
podman run --rm -it -v "$(pwd):/work:z" meshmc-build

# Inside the container:
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The container comes with Qt 6.10.2 (installed via `aqtinstall`), Clang, LLD,
Ninja, CMake, and all required dependencies pre-installed.

Note that this invocation configures without a preset, so it does **not** use
the vcpkg toolchain: the dependencies are resolved against the packages already
present in the image.

## Running Tests

### Using CTest Presets

```bash
# Configure first (if not done)
cmake --preset linux

# Build including test targets
cmake --build --preset linux --config Debug

# Run tests
ctest --preset linux --build-config Debug
```

### Running Tests Directly

```bash
cd build
ctest --output-on-failure
```

### Available Test Binaries

After building, individual test binaries are available in `build/`:

- `DownloadTask_test`
- `FileSystem_test`
- `GradleSpecifier_test`
- `GZip_test`
- `Index_test`
- `INIFile_test`
- `JavaVersion_test`
- `Library_test`
- `ModFolderModel_test`
- `MojangVersionFormat_test`
- `ParseUtils_test`
- `UpdateChecker_test`
- `sys_test`

## CMake Options

These options can be set during configuration with `-D<OPTION>=<VALUE>`:

| Option                         | Default  | Description                                |
|--------------------------------|----------|--------------------------------------------|
| `ENABLE_LTO`                   | `OFF`\*  | Enable Link Time Optimization              |
| `MeshMC_QT_VERSION_MAJOR`     | `6`      | Major Qt version to build against: `6` (>= 6.4) or `5` (>= 5.15). Use a separate build directory per value — see [Choosing the Qt version](#choosing-the-qt-version) |
| `MeshMC_BUILD_PLATFORM`       | `""`     | Platform identifier string (display only)  |
| `MeshMC_BUILD_ARTIFACT`       | `""`     | Legacy substring used to match a feed asset when structured attributes are unavailable |
| `MeshMC_BUILD_PLATFORM_ID`    | `""`     | Updater asset platform id: `linux` / `windows` / `macos` |
| `MeshMC_BUILD_ARCH`           | `""`     | Updater asset arch: `x86_64` / `aarch64`   |
| `MeshMC_BUILD_PORTABLE`       | `""`     | Updater asset portable flag: `true` / `false` |
| `MeshMC_BUILD_KIND`           | `""`     | Updater asset kind: `archive` / `appimage` / `installer` |
| `MeshMC_META_URL`             | (set)    | URL for meta server                        |
| `MeshMC_NEWS_RSS_URL`         | (set)    | URL for news RSS feed                      |
| `MeshMC_UPDATER_BASE`         | `""`     | Legacy GoUpdate base URL (unused)          |
| `MeshMC_UPDATER_FEED_URL`     | (set)    | Authoritative product feed URL             |
| `MeshMC_UPDATER_LATEST_JSON_URL` | (set) | Cross-check mirror URL (empty disables it) |
| `MeshMC_NOTIFICATION_URL`     | (set)    | URL for notifications                      |
| `MeshMC_PASTE_EE_API_KEY`     | (set)    | paste.ee API key                           |
| `MeshMC_IMGUR_CLIENT_ID`      | (set)    | Imgur API client ID                        |
| `MeshMC_CURSEFORGE_API_KEY`   | (set)    | CurseForge API key                         |
| `BUILD_TESTING`                | `ON`     | Build unit tests                           |

> \* `ENABLE_LTO` defaults to `OFF` in the CMakeLists.txt, but presets set it
> to `ON`.

### Example

```bash
cmake --preset linux \
    -DENABLE_LTO=OFF \
    -DBUILD_TESTING=OFF \
    -DMeshMC_BUILD_PLATFORM="linux-x86_64"
```

## Troubleshooting

### In-Source Builds are Forbidden

CMake will refuse to configure if the source and build directories are the same.
Always use a separate build directory (the presets handle this automatically with `build/`).

### WSL is Not Supported

Building under Windows Subsystem for Linux is explicitly blocked. Use a native
Linux environment, the Windows presets, or a container.

### LTO Failures

If you get linker errors with LTO enabled, disable it:

```bash
cmake --preset linux -DENABLE_LTO=OFF
```

### Missing Qt

If CMake cannot find Qt, make sure the development packages for the Qt major
version you asked for are installed, and that Qt's bin directory is in your
`PATH` (or set `CMAKE_PREFIX_PATH`):

```bash
cmake --preset linux -DCMAKE_PREFIX_PATH=/path/to/qt6

# Qt 5
cmake --preset linux -DMeshMC_QT_VERSION_MAJOR=5 \
    -DCMAKE_PREFIX_PATH=/path/to/qt5
```

Configure prints the version it resolved (`Building against Qt ... (major
version ...)`). If that line names a different major version than you
expected, `CMAKE_PREFIX_PATH` is pointing at the other Qt.

A missing `NetworkAuth` is the most common failure on Qt 5, since most
distros ship it as its own package (`qt5-qtnetworkauth-devel`,
`libqt5networkauth5-dev`) rather than as part of qtbase.

### Missing ECM (Extra CMake Modules)

With a preset, vcpkg installs ECM as a host dependency and this should not
happen — check that `cmake/vcpkg` is actually checked out.

When configuring **without** the vcpkg toolchain, ECM has to come from your
package manager:

- **Debian/Ubuntu:** `sudo apt-get install extra-cmake-modules`
- **Fedora:** `sudo dnf install extra-cmake-modules`
- **Arch:** `sudo pacman -S extra-cmake-modules`
- **macOS:** `brew install extra-cmake-modules`

### Submodule Errors

If CMake cannot find the vcpkg toolchain file
(`cmake/vcpkg/scripts/buildsystems/vcpkg.cmake`), or complains about missing
files under `libraries/`, the submodule is not checked out:

```bash
git submodule update --init --recursive
```
