cmake_minimum_required(VERSION 3.15)
project(MapDrawer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

add_executable(MapDrawer
    src/main.cpp
    src/app_types.cpp
    src/editor_commands.cpp
    src/editor_history.cpp
    src/editor_state.cpp
    src/gl_lite.cpp
    src/grid_geometry.cpp
    src/logger.cpp
    src/mesh_buffer.cpp
    src/project_document.cpp
    src/render_geometry.cpp
    src/shader_program.cpp
    src/tiny_font.cpp
    src/ui_geometry.cpp
)

target_include_directories(MapDrawer PRIVATE src)
target_link_libraries(MapDrawer PRIVATE glfw opengl32 comdlg32)

if (MSVC)
    target_compile_options(MapDrawer PRIVATE /W4)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT MapDrawer)
endif()

include(CTest)
if (BUILD_TESTING)
    add_executable(MapDrawerCoreTests
        tests/core_tests.cpp
        src/app_types.cpp
        src/editor_commands.cpp
        src/editor_history.cpp
        src/editor_state.cpp
        src/gl_lite.cpp
        src/grid_geometry.cpp
        src/logger.cpp
        src/mesh_buffer.cpp
        src/project_document.cpp
        src/render_geometry.cpp
        src/shader_program.cpp
        src/tiny_font.cpp
        src/ui_geometry.cpp
    )
    target_include_directories(MapDrawerCoreTests PRIVATE src)
    target_link_libraries(MapDrawerCoreTests PRIVATE glfw opengl32)
    if (MSVC)
        target_compile_options(MapDrawerCoreTests PRIVATE /W4)
    endif()
    add_test(NAME MapDrawerAllFeatures COMMAND MapDrawerCoreTests --headless)
    set(MAP_DRAWER_TEST_FEATURES
        grid terrain elevation fog regions markers routes dungeons tools commands
        history state presentation persistence compatibility validation lookups
    )
    foreach(FEATURE IN LISTS MAP_DRAWER_TEST_FEATURES)
        add_test(NAME MapDrawerFeature_${FEATURE}
                 COMMAND MapDrawerCoreTests --feature ${FEATURE})
        set_tests_properties(MapDrawerFeature_${FEATURE} PROPERTIES LABELS "feature")
    endforeach()
endif()
