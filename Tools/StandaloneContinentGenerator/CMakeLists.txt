cmake_minimum_required(VERSION 3.20)
project(SolarOrbzStandalone CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SOLARORBZ_BUILD_GUI "Build the raylib-based live 3D preview GUI" ON)

include(FetchContent)

# --- nlohmann/json (single header, CLI + GUI config parsing) ---
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

# --- stb (single-header stb_image_write.h, PNG output for the CLI) ---
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(stb)
add_library(stb_image_write INTERFACE)
target_include_directories(stb_image_write INTERFACE ${stb_SOURCE_DIR})

# --- Core: portable generation math, no UE/UObject types, shared by CLI and GUI ---
add_library(solarorbz_core STATIC
  src/core/NoiseBasis.cpp
  src/core/ContinentLayer.cpp
)
target_include_directories(solarorbz_core PUBLIC src/core)
target_link_libraries(solarorbz_core PUBLIC nlohmann_json::nlohmann_json)

# --- CLI: config in, heightmap PNG + stats out ---
add_executable(solarorbz_cli src/cli/main.cpp)
target_link_libraries(solarorbz_cli PRIVATE solarorbz_core stb_image_write)

# --- GUI: raylib + raygui live 3D preview ---
if(SOLARORBZ_BUILD_GUI)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(raylib)

  FetchContent_Declare(
    raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 4.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(raygui)
  add_library(raygui_header INTERFACE)
  target_include_directories(raygui_header INTERFACE ${raygui_SOURCE_DIR}/src)

  add_executable(solarorbz_gui src/gui/main.cpp)
  target_link_libraries(solarorbz_gui PRIVATE solarorbz_core raylib raygui_header)
  target_compile_definitions(solarorbz_gui PRIVATE PLATFORM_DESKTOP)
endif()
