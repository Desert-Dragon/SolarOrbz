# Quick Build Guide

This is a standalone CMake project, independent of the SolarOrbz UE plugin build - no Unreal Editor
or `.uproject` required. See `README.md` for what it does and its scope cuts; this file is just the
compile steps.

## Requirements

- CMake 3.20+
- A C++17 compiler (GCC, Clang, or MSVC)
- Network access the first time you configure (dependencies are fetched automatically via CMake
  `FetchContent`: nlohmann/json, stb, and for the GUI, raylib + raygui)
- **Linux only, GUI build:** X11 development headers for raylib's windowing backend. On
  Debian/Ubuntu:
  ```sh
  sudo apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libxkbcommon-dev mesa-common-dev libgl1-mesa-dev
  ```
  (Not needed for `-DSOLARORBZ_BUILD_GUI=OFF`, CLI-only builds.)

## Build

From this directory:

```sh
mkdir build && cd build
cmake ..
cmake --build . -j
```

This produces `solarorbz_cli` and `solarorbz_gui` in `build/`. The first configure takes a few
minutes (cloning raylib); later configures are instant since `FetchContent` caches under `build/_deps/`.

To skip the GUI entirely (CLI only, no raylib/raygui fetch, no X11 headers needed):

```sh
cmake -DSOLARORBZ_BUILD_GUI=OFF ..
cmake --build . -j
```

## Run

```sh
./solarorbz_cli --out heightmap.png                 # CLI: writes a heightmap PNG + prints stats
./solarorbz_gui                                     # GUI: interactive 3D preview
```

See `README.md` for the full flag list, JSON config format, and GUI controls.

## Troubleshooting

- **`RandR headers not found` (or similar GLFW/X11 configure error) on Linux** - install the X11 dev
  packages listed above, then re-run `cmake ..` (delete `build/` first if it already has a stale
  `CMakeCache.txt` from the failed attempt).
- **GUI window doesn't appear / `DISPLAY` errors** - `solarorbz_gui` needs a real X11 (or Wayland via
  XWayland) display; it won't run in a headless SSH session without one (e.g. `Xvfb` + a VNC viewer,
  or just run it on your desktop).
