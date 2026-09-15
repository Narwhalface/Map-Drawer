# Map Drawer

Map Drawer is a C++17/OpenGL map editor for tabletop campaigns. It provides square and hex
overworld maps, configurable terrain, elevation, fog of war, settlements, points of interest,
encounters, routes, and dungeon maps linked to Dungeon POIs.

## Requirements

Map Drawer currently builds on Windows. Install the following before building:

- [Git for Windows](https://git-scm.com/download/win). CMake uses Git to download GLFW.
- [CMake](https://cmake.org/download/) 3.15 or newer. Select the installer option that adds
  CMake to `PATH`.
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) or Visual Studio 2022
  Build Tools with the **Desktop development with C++** workload installed.
- A graphics driver with OpenGL 3.3 support.

An internet connection is required during the first CMake configuration because GLFW 3.4 is
downloaded automatically.

## Download the source

Open PowerShell and run:

```powershell
git clone https://github.com/Narwhalface/Map-Drawer.git
Set-Location "Map-Drawer"
```

If you already have the source, open PowerShell in the project directory instead.

## Build

Run these commands from the directory containing `CMakeLists.txt`:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
```

CMake downloads and builds GLFW automatically. A successful build creates:

```text
build\Release\MapDrawer.exe
```

If CMake cannot find a compiler, open **Developer PowerShell for VS 2022**, return to the project
directory, and run the same commands there. Also confirm that the Visual Studio C++ workload is
installed.

## Run

The recommended method is to launch Map Drawer from the project directory:

```powershell
.\build\Release\MapDrawer.exe
```

You can also open `build\Release` in File Explorer and double-click `MapDrawer.exe`. Files such as
projects, autosaves, backups, screenshots, configuration, and logs are stored relative to the
directory from which the application is launched. Starting it from the project directory keeps
those files together at the repository root.

Use the in-application **HELP** button or press `Shift+/` (`?`) to view the complete controls and
keybindings.

## Run the tests

After building, run:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

The test executable is also available directly at:

```text
build\Release\MapDrawerCoreTests.exe
```

It tests project-file persistence, compatibility, grid geometry, and core domain behaviour. It
does not launch the graphical editor.

## Rebuild after changing the source

For normal source changes, only the build command is needed:

```powershell
cmake --build build --config Release
```

Run the initial `cmake -S . -B build -DBUILD_TESTING=ON` command again after changing
`CMakeLists.txt` or if the build directory has been removed.

## Troubleshooting

- **`cmake` is not recognized:** install CMake, add it to `PATH`, and reopen PowerShell.
- **No C++ compiler was found:** install the Visual Studio **Desktop development with C++**
  workload or use Developer PowerShell for VS 2022.
- **GLFW cannot be downloaded:** check the internet connection and make sure Git is available from
  PowerShell with `git --version`.
- **The application cannot create an OpenGL window:** update the graphics driver and verify that
  the computer supports OpenGL 3.3.
- **A project or autosave appears to be missing:** check the directory from which `MapDrawer.exe`
  was launched, especially `build\Release` if it was opened by double-clicking.

## Source layout

- `src/main.cpp` coordinates input, editor operations, the GUI, and rendering.
- `src/app_config.h` contains application limits, tuning values, and file names.
- `src/app_types.*` defines the domain model, palettes, and editor enums.
- `src/grid_geometry.*` contains square and hex grid calculations.
- `src/project_document.*` reads, writes, and validates project files.
- `tests/core_tests.cpp` tests non-graphical core behaviour.
