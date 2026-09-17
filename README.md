# Map Drawer

Map Drawer is a C++17/OpenGL map editor for tabletop campaigns. It provides square and hex
overworld maps, configurable terrain, elevation, fog of war, settlements, points of interest,
routes, and dungeon maps linked to Dungeon POIs. Its structured encounter builder supports
noncombat events, optional creature groups, ordered effect chains, and one or more editable
die-result tables with From, To, and Result columns.
World maps, dungeon maps, encounters, and creature stat blocks can each be created as standalone
files from the main menu. Encounter files can import saved creature files into their creature list.
Encounters can also be placed on world maps. Creature stat blocks start with zero special abilities
and can add any number of named abilities. Core STR, DEX, CON, INT, WIS, and CHA scores use
separate labeled numeric boxes, so only their values need to be entered.

Standalone encounters can be linked into world maps and dungeon tiles. The encounter runner keeps
round, turn, hit-point, condition, and defeated-state information in the project file, while roll
tables can be rolled directly from the builder. Dungeon maps support typed GM markers for rooms,
encounters, traps, treasure, secrets, stairs, portals, and notes. Creature records also preserve
defenses, proficiency, passive perception, spellcasting, and portrait/token references. The project
sidebar provides Save As and the main menu can reopen the most recent file with Continue Last.

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
keybindings. On startup, choose the world-map editor, create a standalone dungeon map, or use the
file picker to load any saved map. Press `Escape` from the world editor to return to this menu.
Dungeon POIs on world maps can either create an embedded dungeon or link a standalone dungeon file.

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

The executable can also list or run individual feature suites:

```powershell
.\build\Release\MapDrawerCoreTests.exe
.\build\Release\MapDrawerCoreTests.exe --list
.\build\Release\MapDrawerCoreTests.exe --feature dungeons
.\build\Release\MapDrawerCoreTests.exe --headless
```

Running it without arguments opens a live graphical dashboard and then executes every suite in
sequence. Rows change from **WAIT** to **RUN**, then **PASS** or **FAIL**. A visual-check panel
simultaneously displays the production tile, fog, marker, route, label, elevation, and UI-button
geometry used by the presentation tests. Press Enter, Escape, or the window close button when
finished. After the final result, the dashboard remains open indefinitely until one of those user
actions closes it. The `--headless` option runs the same complete test without opening a window.
CTest uses headless mode and registers each feature separately, so automated runs cannot wait for
user input and failures identify the affected area.

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

- `src/main.cpp` coordinates the window lifecycle, input, high-level tools, and frame assembly.
- `src/app_config.h` contains application limits, tuning values, and file names.
- `src/app_types.*` defines the domain model, palettes, and editor enums.
- `src/editor_state.*` owns document-level editor state and lifecycle invalidation.
- `src/editor_commands.*` provides world/dungeon layer reads, writes, and capacity management.
- `src/editor_history.*` owns stroke recording and undo/redo independently of the window system.
- `src/grid_geometry.*` contains square and hex grid calculations.
- `src/project_document.*` reads, writes, and validates project files.
- `src/ui_geometry.*` generates UI primitives and hit targets without depending on OpenGL.
- `src/render_geometry.*` generates camera-transformed map, route, marker, and label geometry.
- `tests/core_tests.cpp` tests persistence, commands, history, geometry, and other non-graphical
  behaviour.
