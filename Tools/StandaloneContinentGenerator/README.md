# SolarOrbz Standalone Continent Generator

A standalone (non-UE) C++ tool for iterating on procedural planet terrain generation without an
Unreal Editor rebuild/reimport cycle in the loop. Started as a port of just the SolarOrbz plugin's
**Continent Layer** (`USolarOrbzContinentTerrainLayer`); has since grown into a genuine
feature-comparable terrain-generation pipeline in its own right - Continent base shape, noise
layering, base-layer adjustment, real particle-based hydraulic erosion, thermal erosion, a
temperature/moisture climate simulation, and Whittaker-style biome classification - independently
reimplemented from public/academic techniques (see "Pipeline" below for sourcing per stage), not
decompiled or reverse-engineered from any commercial tool's code.

Two frontends share one core library:

- **`solarorbz_cli`** - config (JSON) in, a set of grayscale/color equirectangular PNGs (height,
  biome, temperature, moisture) + land coverage/biome-mix/simulation stats out. Good for batch
  generation or scripting.
- **`solarorbz_gui`** - an interactive real-time 3D orbit-camera preview (raylib + raygui) with live
  sliders for every pipeline stage, "Run Hydraulic Erosion"/"Run Thermal Erosion"/"Run Simulation"
  buttons for the stages too expensive to re-run on every slider drag, and a Height/Biome view toggle.

## Pipeline

Generation is a linear, ordered stack of nodes (`TerrainPipeline`, `src/core/TerrainNode.h`) baked
onto one shared `FHeightField` (`src/core/HeightField.h` - an equirectangular grid; X wraps around
the sphere, Y clamps at the poles, a deliberate planar-grid simplification over the UE plugin's own
icosphere, same "accepted pole pinching" trade-off as the preview mesh below). This deliberately
mirrors the UE plugin's own documented model (`Docs/SolarOrbzTerrainGraphEditor.md`: "the graph is a
transient VIEW over a linear stack") rather than building a full visual node-graph UI, which is out
of scope here.

| Stage | Node | Source technique |
|---|---|---|
| Base shape | `ContinentNode` | The ported Continent Layer (Radial Seeds / Voronoi Growth / Plate Tectonics) |
| Additive detail | `NoiseNode` (one or more) | Fractal (fBm) Perlin/Ridged/Billow/Value/Voronoi noise |
| Base-layer adjust | `HeightAdjustNode` | Scale / offset / power-curve remap of the whole field |
| (built, not yet wired) | `CombineNode` | Secondary-noise blend (Add/Subtract/Multiply/Max/Min/Average) |
| Hydraulic erosion | `HydraulicErosionNode` | Particle/"droplet" erosion (Beyer, *Implementation of a method for hydraulic erosion*, TU Munich B.Sc. thesis, 2015; the same lineage as Sebastian Lague's widely-reimplemented tutorial version) |
| Thermal erosion | `ThermalErosionNode` | Talus-angle mass redistribution (Musgrave, Kolb & Mace, SIGGRAPH 1989) |
| Climate | `SimulateClimate` (`Climate.h`) | Latitude + elevation-lapse temperature; prevailing-wind sweep with orographic lift/rain-shadow for moisture (same academic lineage as Azgaar's Fantasy Map Generator) |
| Biome | `ClassifyBiomes` (`BiomeClassifier.h`) | Whittaker (1975) height/temperature/moisture biome diagram, with height-based special cases (ocean/beach/snow-ice/bare-mountain) |

Erosion is **not** part of the live-rebuild pipeline - tens of thousands of droplets, or 20 iterations
of thermal diffusion, over the whole grid is too expensive to re-run on every slider drag. Both run
explicitly via their own GUI buttons (or the CLI's `HydraulicErosion`/`ThermalErosion` config keys)
directly onto the currently-baked terrain; "Regenerate" discards any applied erosion and rebuilds
from Continent/Noise/HeightAdjust only.

**A note on erosion defaults:** unlike the textbook version of the droplet algorithm (which runs over
a height field normalized to 0-1, with a small separate "strength" constant), this field's heights
are already real meters, so the erosion math is naturally meters-scale on its own before any extra
scaling is applied. `FHydraulicErosionConfig`'s defaults (`NumDroplets`, `ErosionStrengthMeters`) were
tuned empirically via CLI (comparing Ocean/Beach/biome-mix % against an unmodified bake, on both a
thin ~200 m Radial Seeds default and a 4000 m-mountain Plate Tectonics bake) to leave a visible-but-
not-destructive effect by default on both - earlier, much higher defaults washed away effectively all
land on both terrain types.

## Scope (read this before comparing to the UE plugin)

Every `FContinentConfig` field is named identically to the UE plugin's own `UPROPERTY` (see
`src/core/ContinentLayer.h`), with the same default values, so a config file reads like the plugin's
Details panel. Explicitly out of scope: the UE plugin's own Heightmap/Stamp/Terrace/Canyon layers and
its node-graph *editor UI* (this tool's pipeline is a fixed C++ stack, not an editable visual graph);
true GPU-accelerated real-time preview (erosion/climate/biome all re-bake on the CPU - see
`ROADMAP.md` for this as a documented, deferred follow-up phase).

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
./solarorbz_cli --config path.json --out-prefix /tmp/myplanet --image-width 1024 --image-height 512
```

Run `./solarorbz_cli --help` for the full flag list (`--config`, `--out-prefix`, `--image-width`,
`--image-height`, `--seed`). The config JSON describes the whole pipeline - every key is optional,
omitted stages/fields keep their defaults:

```json
{
  "Seed": 42,
  "GridWidth": 256,
  "GridHeight": 128,
  "Continent": { "Algorithm": "PlateTectonics", "NumPlates": 16, "MountainHeightMeters": 6000.0 },
  "AdvanceSteps": 20,
  "AdvanceMyr": 10,
  "Noise": [ { "Amplitude": 150.0, "Frequency": 4.0 } ],
  "HeightAdjust": { "ScaleMultiplier": 1.0, "PowerCurve": 1.0 },
  "HydraulicErosion": { "NumDroplets": 2000 },
  "ThermalErosion": { "Iterations": 20 },
  "Climate": { "EquatorTempC": 28.0, "PoleTempC": -35.0 },
  "Biome": {}
}
```

`Continent`/`Noise`/`HeightAdjust` fields are named identically to their `FContinentConfig`/
`FNoiseNodeConfig`/`FHeightAdjustConfig` C++ struct fields; `HydraulicErosion`/`ThermalErosion`/
`Climate`/`Biome` likewise match their own config structs (see "Pipeline" above for the header each
lives in). Output: `<out-prefix>_height.png` (a sea-level-split grayscale heightmap - ocean and land
are normalized separately around a fixed sea-level midpoint, since a real planet's ocean-trench vs.
eroded-land relief span wildly different magnitude ranges and a single linear min/max normalization
crushes land into an indistinguishable sliver), `<out-prefix>_biome.png` (biome color map),
`<out-prefix>_temperature.png`, `<out-prefix>_moisture.png`, plus land coverage %, per-biome mix %,
and (Plate Tectonics only) simulated age and live plate count, printed to stdout.

## GUI usage

Run `./solarorbz_gui`. The preview uses a visualization-only radius (400 km, deliberately not
Earth-scale) chosen so the default Continent Layer height values are comfortably visible without
distorting the sphere - raylib's default far clip plane is also widened at startup to fit this scale
(see `rlSetClipPlanes` in `src/gui/main.cpp`). Left-drag to orbit the camera, scroll to zoom.

The preview mesh resolution matches the pipeline's own bake resolution (256x128), and vertex colors
get a simple fixed-direction Lambertian shading term baked in on top of the elevation/biome color
ramp. Both fixed real, confirmed-via-screenshot issues, not just polish: a coarser preview mesh was
blurring/aliasing away the Secondary Noise layer's hill/mountain-scale detail (identical config looked
flat in 3D but showed clear mottled relief in the CLI's heightmap PNG, which samples the baked field
directly, at the same resolution); and raylib's default material is unlit, so terrain slopes carried
real data but never actually looked three-dimensional - the flat elevation-ramp color over the lat/
long mesh's regular diagonal-split triangulation also produced a visible crosshatch moire wherever the
color had any local contrast, which the shading incidentally smooths over too.

The left panel follows the pipeline order:

- **Planet** - **Radius (km)**: the actual planet radius the preview sphere is built at (not a
  visualization fake-out - `BuildSphereMesh` displaces real vertices against this exact value), range
  1 km to 100,000 km (small moons through gas-giant scale). **Grid Resolution**: the pipeline's bake
  resolution and the preview mesh resolution, kept in lockstep, range 64-352 (always 2:1 width:height;
  capped so vertex count stays under the preview mesh's `unsigned short` index limit). Both rebuild
  the preview (Radius refreshes the mesh at the new scale and rescales the camera to keep the same
  relative zoom; Grid Resolution reallocates the mesh buffers and re-bakes the whole pipeline, same as
  Regenerate).
- **Algorithm** - the Continent Layer's own sliders (same as before), switching to Plate Tectonics
  adds Num Plates/Mountain Height/Trench Depth/Steps Per Run/Myr Per Step plus a **Run Simulation**
  button that advances the persistent plate state and rebakes.
- **Base Layer Adjust** - Scale/Offset/Power Curve, applied to the whole baked field.
- **Secondary Noise** - an additive fractal noise layer (Amplitude/Frequency). **On by default**
  (350m/8.0) - the Continent layer alone only shapes coastlines/plate-boundary-scale relief, so
  without this the interior of any landmass is smooth and featureless; this is the layer that
  actually puts hills and mountains on the land (still a live slider, so it can be turned back to 0
  for a smooth/stylized look).
- **Erosion (explicit - click to apply)** - Droplets/Hydraulic Rate sliders plus a **Run Hydraulic
  Erosion** button, and Thermal Iterations/Talus Angle sliders plus a **Run Thermal Erosion** button.
  Both apply directly onto the current baked terrain rather than re-running on every slider drag (see
  "Pipeline" above) - **Regenerate** discards any applied erosion.
- **Climate** - Equator/Pole Temp, Ocean Evaporation, Rain Shadow Sensitivity; recomputes temperature/
  moisture/biome without discarding erosion.
- **View** - Height/Biome toggle for the sphere's surface coloring, plus the visualization-only
  **Height Exaggeration** slider (not part of any config - lets you see relief that would otherwise be
  invisible at true Earth-like scale).

The bottom of the panel shows live Land coverage % (computed directly from the current baked height
field, so it reflects Noise/HeightAdjust/erosion too, not just the raw Continent output).

**A note on slider responsiveness:** every slider in this panel now actually triggers a live rebuild
when dragged. It didn't before - `GuiSlider`'s own `int` return value is dead code in this build of
raygui (`GuiSliderPro` declares `int result = 0;` and never reassigns it), so the
`if (GuiSlider(...)) { *bDirtyFlag = true; }` pattern every slider in this file used was silently
never true. The slider's underlying value still updated correctly (raygui mutates it directly via the
pointer), so this bug was easy to miss in a screenshot of a slider's current position, but no amount
of dragging ever actually re-baked the terrain. Fixed by comparing the value before/after the
`GuiSlider` call ourselves instead of trusting its return.

## Repository layout

```
src/core/           Portable generation math - zero UE/UObject dependencies
  Vec3.h              Double-precision 3D vector + math utils (stands in for FVector)
  Random.h            Seeded RNG (stands in for FRandomStream - see fidelity note above)
  LatLongGrid.h        Equirectangular grid walking/sampling (ports FSolarOrbzLatLongGrid)
  NoiseBasis.h/.cpp    Perlin/Ridged/Billow/Value/Voronoi noise + fractal (fBm) accumulation
  ContinentLayer.h/.cpp  The main port: all 3 algorithms + the tectonic simulation
  HeightField.h       Equirectangular height grid shared by every pipeline node (X wraps, Y clamps)
  TerrainNode.h        ITerrainNode interface + TerrainPipeline (the linear node stack)
  Climate.h           Temperature/moisture simulation over a baked FHeightField
  BiomeClassifier.h    Whittaker-style height/temperature/moisture -> biome classification
  Nodes/              ContinentNode, NoiseNode, HeightAdjustNode, CombineNode,
                      HydraulicErosionNode(.cpp), ThermalErosionNode(.cpp)
  SphereMesh.h        Builds a displayable mesh from any direction->height sampler (GUI only)
src/cli/main.cpp     CLI frontend
src/gui/main.cpp     GUI frontend (raylib + raygui)
```
