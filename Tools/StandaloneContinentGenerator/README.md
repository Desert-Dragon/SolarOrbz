# SolarOrbz Standalone Continent Generator

A standalone (non-UE) C++ tool for iterating on the SolarOrbz plugin's **Continent Layer**
generation math (`USolarOrbzContinentTerrainLayer` in `Source/SolarOrbz/Private/SolarOrbzTerrainLayers.cpp`)
without an Unreal Editor rebuild/reimport cycle in the loop. Built because the UE plugin's default
settings produce terrain relief that's a very small percentage of Earth-scale planet radii (see
`ROADMAP.md`/chat history for the original "why can't I see any mountains" investigation) - this tool
lets you scrub Continent Layer parameters and immediately see the effect, with its own height
exaggeration slider independent of true scale.

Two frontends share one core library:

- **`solarorbz_cli`** - config (JSON) in, a grayscale equirectangular heightmap PNG + land
  coverage/simulation stats out. Good for batch generation or scripting.
- **`solarorbz_gui`** - an interactive real-time 3D orbit-camera preview (raylib + raygui) with live
  sliders for every major Continent Layer property and a "Run Simulation" button for the Plate
  Tectonics algorithm.

## Scope (read this before comparing to the UE plugin)

This is a v1 port of **only** the Continent Layer and its Plate Tectonics simulation (persistent
plates, mountain-building stress, plate splitting) - all three algorithms (Radial Seeds, Voronoi
Growth, Plate Tectonics). It deliberately does **not** port: Noise/Heightmap/Stamp/Erosion/Terrace/
Canyon layers, Climate Simulation, or the Biome system. Every `FContinentConfig` field is named
identically to the UE plugin's own `UPROPERTY` (see `src/core/ContinentLayer.h`), with the same
default values, so a config file reads like the plugin's Details panel.

Three deliberate fidelity trade-offs, each documented at its source:

1. **RNG is not bit-identical to UE.** `src/core/Random.h` wraps `std::mt19937`, not UE's actual
   `FRandomStream` algorithm/constants. A given `Seed` reproduces the same algorithm *shape*, not a
   pixel-identical layout to the UE plugin.
2. **Perlin noise is a different implementation.** UE's `FMath::PerlinNoise3D` is Epic-internal and
   not independently reproducible; `src/core/NoiseBasis.cpp` uses the public-domain Ken Perlin 2002
   reference implementation instead - same noise character, not bit-identical output.
3. **The preview mesh is a plain lat/long UV-sphere, not the UE plugin's icosphere.** `src/core/SphereMesh.h`
   builds directly off `FLatLongGrid` rather than porting `FSolarOrbzIcoSphereGenerator`'s
   subdivision/UV-seam/pentagon-pole handling - it pinches at the poles, same as any naive UV-sphere.
   Fine for a fast-iteration preview tool; not usable as a final-mesh exporter.

## Building

Requires CMake 3.20+, a C++17 compiler, and network access (dependencies are fetched via CMake
`FetchContent`: nlohmann/json, stb, and for the GUI, raylib + raygui).

```sh
mkdir build && cd build
cmake ..                          # add -DSOLARORBZ_BUILD_GUI=OFF to skip the GUI (CLI only, no raylib/raygui fetch)
cmake --build . -j
```

Produces `solarorbz_cli` and (unless disabled) `solarorbz_gui` in the build directory.

## CLI usage

```sh
./solarorbz_cli --algorithm PlateTectonics --advance-steps 20 --advance-myr 10 \
  --out heightmap.png --image-width 1024 --image-height 512
```

Run `./solarorbz_cli --help` for the full flag list. `--config path.json` loads a JSON file whose
keys are `FContinentConfig` field names (any subset - omitted fields keep their default), e.g.:

```json
{
  "Seed": 42,
  "Algorithm": "PlateTectonics",
  "NumPlates": 16,
  "MountainHeightMeters": 6000.0
}
```

Output: a grayscale PNG (black = `min(OceanFloorDepthMeters, TrenchDepthMeters, RiftDepthMeters)`,
white = `max(LandPlateauHeightMeters, MountainHeightMeters, RidgeHeightMeters)`) plus land coverage %,
and (Plate Tectonics only) simulated age and live plate count, printed to stdout.

## GUI usage

Run `./solarorbz_gui`. The preview uses a visualization-only radius (400 km, deliberately not
Earth-scale) chosen so the default Continent Layer height values are comfortably visible without
distorting the sphere - raylib's default far clip plane is also widened at startup to fit this scale
(see `rlSetClipPlanes` in `src/gui/main.cpp`). Left-drag to orbit the camera, scroll to zoom. The left panel exposes the
most load-bearing Continent Layer properties as live sliders (edits rebake and redraw immediately);
switching the Algorithm dropdown to Plate Tectonics adds Num Plates/Mountain Height/Trench Depth/
Steps Per Run/Myr Per Step plus a **Run Simulation** button, which advances the persistent plate
state and rebakes - exactly like the UE plugin's own Run Simulation button, but without needing the
editor open. A separate **Height Exaggeration** slider (visualization-only, not part of
`FContinentConfig`) lets you actually see relief that would otherwise be invisible at true
Earth-like scale.

## Repository layout

```
src/core/           Portable generation math - zero UE/UObject dependencies
  Vec3.h              Double-precision 3D vector + math utils (stands in for FVector)
  Random.h            Seeded RNG (stands in for FRandomStream - see fidelity note above)
  LatLongGrid.h        Equirectangular grid walking/sampling (ports FSolarOrbzLatLongGrid)
  NoiseBasis.h/.cpp    Perlin/Ridged/Billow/Value/Voronoi noise + fractal (fBm) accumulation
  ContinentLayer.h/.cpp  The main port: all 3 algorithms + the tectonic simulation
  SphereMesh.h        Builds a displayable mesh from a FLatLongGrid sample (GUI only)
src/cli/main.cpp     CLI frontend
src/gui/main.cpp     GUI frontend (raylib + raygui)
```
