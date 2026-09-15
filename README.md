# Map Drawer

Map Drawer is a C++17/OpenGL overworld editor for tabletop campaigns. It supports sparse square
and hex maps, customizable terrain palettes, terrain and elevation painting, regions, fog of war, cities, points of interest,
editable DM encounter markers, routes, selection tools, autosave, backups, and versioned project
files.

## Build

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The application is produced at `build/Release/MapDrawer.exe`.

## Source layout

- `main.cpp` coordinates GLFW input, editor operations, UI construction, and render passes.
- `app_config.h` contains application limits, tuning values, and file names.
- `app_types.*` defines the domain model, editor enums, palettes, and display-name lookups.
- `grid_geometry.*` owns square/hex coordinate conversion, neighbors, bounds, and distance logic.
- `project_document.*` owns the versioned project format and validates before replacing live data.
- `mesh_buffer.*` manages OpenGL vertex-array/buffer pairs used by render passes.
- `shader_program.*` compiles and links shaders with consistent error reporting.
- `tiny_font.*` contains the small bitmap font used by map and UI labels.
- `gl_lite.*` loads the small OpenGL API subset needed by the application.
- `logger.*` provides timestamped application logging.

## Design notes

Persistent world data is grouped in `ProjectDocument`. Loading first parses into a temporary
document, so malformed files cannot partially replace the open map. Editing and rendering state
remain transient and are reset explicitly after a successful load.

Grid geometry is stateless and takes the grid type as an argument. This keeps square/hex math
independent from GLFW and OpenGL and allows it to be covered by the core test executable.
