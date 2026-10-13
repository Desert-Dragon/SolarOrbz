#!/usr/bin/env bash
# Quick build script for Linux. Usage:
#   ./build.sh            # CLI + GUI
#   ./build.sh --no-gui   # CLI only (skips the raylib/raygui fetch, no X11 dev headers needed)
set -e

cd "$(dirname "$0")"

BUILD_GUI=ON
for arg in "$@"; do
    case "$arg" in
        --no-gui) BUILD_GUI=OFF ;;
        *) echo "Unknown argument: $arg (expected --no-gui)"; exit 1 ;;
    esac
done

mkdir -p build
cd build

cmake -DSOLARORBZ_BUILD_GUI="$BUILD_GUI" ..
cmake --build . -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "Build complete. Binaries:"
echo "  build/solarorbz_cli"
if [ "$BUILD_GUI" = "ON" ]; then
    echo "  build/solarorbz_gui"
fi
