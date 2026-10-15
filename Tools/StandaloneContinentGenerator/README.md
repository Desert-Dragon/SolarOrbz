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
   Fine for a fast-iteration preview tool; not usable as a final-mesh exporter. One consequence of the
   pinch that isn't just cosmetic: real physical cell separation near either pole shrinks toward zero
   well before the exact pole row, so a height source with no awareness of that distortion in index
   space (`HydraulicErosionNode` especially) can produce per-cell differences that read as an
   enormous local slope once radially displaced - confirmed as a "sunburst" of needle spikes
   converging on a pole after running erosion. `BuildSphereMesh` smooths this out itself: every row
   within ~14.5 degrees of polar angle from either pole blends each cell's height toward its own row
   average, weighted by how compressed that row actually is. Purely a mesh-building smoothing step -
   the underlying `FHeightField` data (what the CLI writes, what Climate/Biome read) is untouched.

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
crushes land into an indistinguishable sliver - **view-only, not reimportable**: see below for that),
`<out-prefix>_biome.png` (biome color map), `<out-prefix>_temperature.png`,
`<out-prefix>_moisture.png`, `<out-prefix>_heightmap_ue5.png` (linear 16-bit, see below),
`<out-prefix>_biomeid_ue5.png` (discrete biome-ID map, see below), `<out-prefix>_pcg_points.csv`
(scatter-point attribute dataset, see below), plus land coverage %, per-biome mix %, and (Plate
Tectonics only) simulated age and live plate count, printed to stdout.

## Bringing a planet into UE5

The real UE plugin already has an import path built for exactly this - `USolarOrbzHeightmapTerrainLayer`
("Heightmap Layer" in the layer stack) samples an equirectangular grayscale `UTexture2D` via
`FSolarOrbzTextureHeightSampler`, remapping black/white to `MinHeightMeters`/`MaxHeightMeters`. This
tool's CLI writes a texture in exactly that shape by default: `<out-prefix>_heightmap_ue5.png`.

**Why not the view-only `_height.png`:** that one is deliberately nonlinear (a sea-level-split
contrast trick for *looking at* a heightmap on screen, where ocean and land are each normalized to
their own half of the range around a fixed midpoint) and only 8 bits deep - reimporting it would
reproduce roughly the right silhouette but NOT the real heights, and would quantize relief to tens of
meters per step. `_heightmap_ue5.png` instead is a plain, linear 0..1 ramp (black = the bake's true
minimum height, white its true maximum - the exact values are printed to stdout as "Wrote (UE5):"),
and genuinely 16-bit (via `lodepng` - `stb_image_write`'s own PNG writer is 8-bit-per-channel only, the
one format already in this project nowhere near precise enough for a real heightmap, which is exactly
why `USolarOrbzHeightmapTerrainLayer`'s own header comment recommends "16-bit or float source" in the
first place). Verified round-trip-correct: decoded the written PNG back with Pillow and confirmed its
minimum/maximum pixel values (0 and 65535) decode to the exact same Min/Max height the CLI printed, to
the hundredth of a meter, with no byte-order or off-by-one error.

Orientation needs no flip or rotation on either side: `FLatLongGrid` (this tool) and
`FSolarOrbzLatLongGrid` (the UE plugin) share the identical "north pole at `V=0`" convention by
construction (this tool is a direct port of that grid), which is the same "north pole at top,
X = longitude" layout `USolarOrbzHeightmapTerrainLayer`'s own header comment documents for the texture
it expects - confirmed directly on a real bake: the written PNG's row 0 (and only row 0) is a single
uniform value across every column, exactly what you'd expect from the north pole (every longitude at
the pole is the same physical point, so of course they bake to the same height) landing at the top of
the image, not partway down it or at the bottom.

Steps:

1. Generate normally: `./solarorbz_cli --config path.json --out-prefix /tmp/myplanet --image-width 2048 --image-height 1024`
   (pick a resolution with the planet's final on-screen size in mind - this is a real texture import,
   not a quick preview, so there's no reason to default to the CLI's normal 1024x512). Note the two
   `Min Height Meters`/`Max Height Meters` values it prints.
2. Drag `myplanet_heightmap_ue5.png` into the Content Browser. In its Texture import settings:
   **Compression Settings**: `None` - exactly what `USolarOrbzHeightmapTerrainLayer`'s own header
   comment already specifies ("Import with compression set to None so full precision survives"), not
   a setting picked here independently. This matters less than it sounds for THIS layer specifically -
   `FSolarOrbzTextureHeightSampler::EnsureDecoded` reads `Texture->Source` (the pristine imported data
   UE keeps alongside the GPU-compressed runtime texture for reimport purposes) directly via
   `GetMipImage`/`CopyTo`, bypassing whatever the GPU/runtime compression produced entirely - its own
   comment says so explicitly ("works regardless of compression settings"). None is still the right
   choice: there's no reason to spend GPU memory/compute compressing a texture this layer never
   actually samples on the GPU. **sRGB**: unchecked - this one DOES matter, independent of Compression
   Settings: `EnsureDecoded` converts the source image to `R32F` preserving `RawImage.GammaSpace`
   as-is rather than forcing linear, specifically so an already-correctly-imported (sRGB off) texture
   round-trips with the exact same numeric values - leaving sRGB on would gamma-skew every height
   value on decode. **Mip Gen Settings**: `NoMipmaps` (an auto-generated mip would blend neighboring
   texels together, quietly smoothing out relief at a distance - harmless for a material's color
   texture, not for a height source sampled directly).
3. Add a **Heightmap Layer** to the planet's terrain layer stack (or `USolarOrbzPlanetSpawnerGraph`'s
   embedded terrain chain), assign the imported texture to **Heightmap Texture**, and set
   **Min Height Meters**/**Max Height Meters** to the two values from step 1 exactly - these aren't
   placeholders or safe defaults, they're the specific range this PNG's black/white actually encode,
   and using different values will scale/shift every height in the import.
4. Regenerate the planet. This one layer now reproduces the full baked pipeline output (Continent +
   Noise + erosion, all of it) as a single authored heightmap - same as temporarily replacing the
   whole live layer stack with this one import, which is the point: iterate on terrain math in this
   fast-rebuild standalone tool, then bring only the final result into the real engine when it's ready,
   without needing every individual layer type (Plate Tectonics simulation, hydraulic/thermal erosion)
   reimplemented or re-tuned by hand inside the Editor.

### Bringing biome data into UE5

The real plugin's biome system (`USolarOrbzClimateBiomeMask`) is a RULE classifier, not a texture
sampler - it re-derives a biome live from Elevation/Latitude/Temperature/Moisture/Slope, the same
Whittaker-diagram approach this tool's own `BiomeClassifier.h` already uses, so there's no "biome
texture" bridge needed for the live classification to work at all once Climate Simulation and Biome
Stack assets are authored in UE with equivalent parameters. What IS missing is getting THIS bake's
specific, already-computed biome result (shaped by whatever Plate Tectonics/Noise/erosion actually
produced, not just a fresh live reclassification) back out: `<out-prefix>_biomeid_ue5.png` - an 8-bit
grayscale PNG where each pixel's raw numeric VALUE is the `EBiome` enum ordinal (0 = Ocean, 1 = Beach,
... - the CLI prints the full legend every run), not a color. **Sample it with point/nearest-neighbor
filtering only** - biomes are discrete categories, and bilinearly blending two adjacent IDs (say, 4
and 9) produces a meaningless "6.5"-th biome that doesn't exist. Same orientation convention as the
heightmap (no flip needed).

## PCG scatter-point dataset

`<out-prefix>_pcg_points.csv` - this is the second part of "bringing a planet into UE5," and the
dataset asked for specifically to drive PCG/PCGEx-based scattering (trees, rocks, wildlife density,
whatever a biome should populate its surface with). It isn't a new, invented convention: the real
plugin's own `USolarOrbzBiome` asset already has an `FSolarOrbzBiomeScatterEntry` array whose header
comment says outright "the PCG/PCGEx graph will read these (matched by Tag) once that hookup exists" -
this CSV, plus a new matching `FSolarOrbzPCGScatterPointRow` DataTable row struct added alongside that
comment in `SolarOrbzBiomeSystem.h`, is that hookup's data side.

One row per cell of the **internal bake grid** (`GridWidth`/`GridHeight` - independent of
`--image-width`/`--image-height`, which only size the texture outputs above), not one row per output
pixel: this is a coarse attribute-SAMPLE grid for a PCG graph to project onto its own much denser,
separately-generated point cloud (e.g. via a surface sampler on the imported heightmap's mesh), not a
literal "one spawned object per CSV row" list - a 256x128 bake (the default) gives 32,768 rows, each
describing one sample point, not 32,768 trees.

Columns, deliberately mirroring `FSolarOrbzBiomeSampleContext`'s own fields rather than inventing a
parallel convention: `Name` (DataTable row key, e.g. `Point_12345` - UE's own CSV-to-DataTable
importer requires the first column to be this, matched by position not header text), `Position` /
`Normal` (UE-unit `FVector`s, Unreal's own `(X=...,Y=...,Z=...)` struct-text format in a single quoted
column each - `Position` is the real displaced surface point, already Radius + Elevation, sampled from
a real `SphereMesh.h` build at `--radius-meters` so Slope benefits from this project's own already-
verified pole-smoothing and centered-difference normals rather than a separately-written, possibly
differently-buggy calculation), `ElevationCm`, `Slope` (0 flat..1 vertical, same convention as
`FSolarOrbzBiomeSampleContext::Slope` - averaged over this grid's own cell spacing, so a cliff
narrower than one sample cell reads gentler than it really is; raise `GridWidth`/`GridHeight` if a
biome's `MinSlope`/`MaxSlope` scatter rule needs finer resolution than the default gives), `TemperatureKelvin`
(converted from this tool's own internal Celsius - `FSolarOrbzBiomeSampleContext::Temperature` is
Kelvin), `Moisture` (0..1, direct), and `BiomeName` (e.g. `Taiga`, `Snow/Ice` - match this against a
`FSolarOrbzBiomeScatterEntry::Tag`, or a Biome asset's own name, in your PCG graph's filter/branch
logic to route each point to that biome's intended scatter rules).

**Caught and fixed before it shipped:** the first version of this export left `Position`/`Normal`
unquoted - since the format itself contains commas (between `X=`/`Y=`/`Z=`), every standards-
conforming CSV reader splits an unquoted field like that into three separate columns instead of one,
silently misaligning every column after it. Confirmed by reading the file back with Python's own
`csv` module (which follows the same RFC 4180 quoting rules any CSV-import feature, including UE's
DataTable importer, does) and watching it happen; fixed by wrapping both fields in double quotes.
Verified the corrected file two ways: re-parsed it with `csv.DictReader` and confirmed all 8 columns
land correctly with sane ranges (Temperature 236.7K..300.7K against a 238.15K/301.15K pole/equator
config - lapse-rate-adjusted, as expected; Slope topping out around 0.13 on this particular bake,
plausible for a coarse 256x128 sample grid even with 4,000m peaks - the steepest slopes land on
trench/cliff WALLS, not at elevation extrema, since a local max/min is where the gradient is zero by
definition, not where it's steepest), and independently recomputed one row's position vector magnitude
by hand (404066.75m) against that row's own Radius + Elevation (400,000m + 4,066.75m) - exact match.

**On the exact PCG/PCGEx node that turns a DataTable into point data:** genuinely could not verify a
specific current node name for this in this environment (no live UE5.8 editor, no PCGEx installed) -
web search turned up Epic's own core PCG framework having external-data/row-to-point machinery
(`FPCGExternalDataContext`) but not a confirmed node name, and PCGEx's own documentation describes it
as doing "data sampling and blending... between point sets, surfaces, splines, and textures" without
surfacing its exact external-file-import mechanics either. What IS fully verified and version-stable:
the CSV itself, and that a code-defined `FTableRowBase`-derived USTRUCT with UPROPERTY names matching
the CSV's header row (case-sensitive) is the long-standing, engine-version-independent way to import a
CSV as a UE DataTable at all - `FSolarOrbzPCGScatterPointRow` does exactly that. From a DataTable of
that type, search your installed engine/PCGEx version's node palette for a Data Table point source;
once points exist with these attributes, any PCGEx (or vanilla PCG) filter node can branch on
`BiomeName` the way described above. This half of the pipeline is left to verify against your actual
installed version rather than asserted as fact here.

## GUI usage

Run `./solarorbz_gui`. The preview uses a visualization-only radius (400 km, deliberately not
Earth-scale) chosen so the default Continent Layer height values are comfortably visible without
distorting the sphere - raylib's default far clip plane is also widened at startup to fit this scale
(see `rlSetClipPlanes` in `src/gui/main.cpp`). Left-drag to orbit the camera, scroll to zoom - both
only over the 3D view; the same mouse wheel scrolls the left panel instead while hovered over it
(needed once Plate Tectonics is selected or Height Exaggeration/stats push the panel's content taller
than the window - a thin scrollbar tracks position along the panel's right edge when there's anything
to scroll).

Two thin reference lines overlay the sphere for orientation: a red line through both poles (the
sphere's own rotation axis), and a yellow great circle through the prime meridian (0 deg longitude) -
the same plane also happens to pass through the antimeridian (180 deg), so one circle shows both for
free. Both are purely visual (no node config, nothing saved) and are drawn at a radius a touch past
the single tallest displaced vertex, so they stay a complete, unbroken line/circle regardless of
Height Exaggeration or Radius - never clipped under even the tallest mountain. Both are built out of
short `DrawCylinderEx` segments, not raylib's `DrawLine3D`/`DrawCircle3D` - an earlier version used
those and, confirmed via screenshot (both on this project's own test setup and the user's own
machine), the far-side half of each shape rendered straight through the opaque terrain instead of
being hidden behind it as expected. Those two functions draw through rlgl's immediate-mode line batch,
a separate path from `DrawModel`'s own mesh draw, and don't reliably depth-test against it. Built as
real mesh triangles instead (the same draw path the sphere itself uses), both now correctly disappear
behind the terrain wherever they're actually behind it.

**A visually unusual but genuine terrain feature, not a rendering bug:** a large, sharp-edged inland
sea/landlocked oceanic plate can appear with Plate Tectonics at higher Num Plates counts, looking
enough like a "hole" or "the far side of the planet showing through" to be mistaken for a backface-
culling/depth-test bug. It isn't - confirmed two ways: the CLI's flat equirectangular heightmap PNG
(which samples the baked `FHeightField` directly, with no 3D mesh/renderer involved at all) reproduces
the same sharp-edged shape at the same seed, and rotating the GUI's camera a full turn around an
affected planet shows a solid, seamless sphere with a *different*, non-duplicated terrain on the far
side - no gap, no bleed-through. The shape's unusually sharp (non-organic) edges are a real artifact
of the Plate Tectonics algorithm's own Voronoi-style plate boundaries not always being softened enough
by Coastline Noise Amplitude/erosion at higher plate counts - a Continent Layer tuning matter, not a
GUI rendering one. Raising Coastline Noise Amplitude, running Hydraulic/Thermal Erosion, or lowering
Num Plates all make such boundaries read as more natural coastlines.

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
- **Secondary Noise** - an additive fractal noise layer: a Perlin/Ridged/Billow/Value/Voronoi toggle
  group plus Amplitude/Frequency. **On by default** (Ridged, 350m/8.0) - the Continent layer alone
  only shapes coastlines/plate-boundary-scale relief, so without this the interior of any landmass is
  smooth and featureless; this is the layer that actually puts hills and mountains on the land (still
  live, so it can be turned back to Amplitude 0 for a smooth/stylized look). **Ridged, not Perlin, is
  the default** - confirmed via CLI heightmap comparison at identical Amplitude/Frequency that Ridged's
  connected, sharp-crested ridgelines are what reads as "mountains," where Perlin's rounded, mottled
  bumps read as rolling hills (Ridged takes `abs(Perlin)` and inverts it, turning the noise field's
  naturally line-like zero-crossings into ridge crests instead of smooth hilltops).
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
`GuiSlider` call ourselves instead of trusting its return. The same dead-return-value issue applies to
`GuiToggleGroup` (used by the Secondary Noise type selector above) - same fix, comparing the bound
`int` before/after the call instead of trusting the control's own return.

**A grid-aligned "plaid" artifact on Ridged-noise mountains, now fixed:** switching Secondary Noise to
Ridged on a Plate Tectonics landmass used to bake in a regular, grid-aligned light/dark lattice pattern
over the terrain - confirmed present directly in the baked `FHeightField` data itself (reproduced in
the CLI's flat equirectangular heightmap PNG, no 3D mesh or lighting involved at all), so it was never
a rendering bug. Two real but unrelated bugs were found and fixed along the way without resolving it:
`NoiseNode`/`CombineNode` were silently dropping multi-octave fBm accumulation for every basis except
Perlin (an `Octaves` slider setting was simply ignored for Ridged/Billow/Value/Voronoi - see
`NoiseBasis::FractalNoiseGeneric`, which now backs every basis the way the UE plugin's own
`ComputeNormalizedNoiseUncompensated` always has), and `SphereMesh`'s per-vertex normals were computed
from one-sided forward differences only (`X+1`/`Y+1`), now centered (`X-1..X+1`/`Y-1..Y+1`) to remove a
directional lighting bias on sharply-varying terrain. The actual root cause: `NoiseBasis::
ComputeSeedOffset(Seed)` - meant to shift every noise layer's sampling domain so it doesn't start
exactly at the Perlin lattice origin - degenerated to the exact zero vector for `Seed=0` specifically
(`sin(0 * k)` is exactly `0` on all three axes, so `Frac(0) == 0`), and `Seed=0` is the pipeline's own
default. With zero offset, Ridged noise sampled starting precisely at classic Perlin's integer lattice
origin along perfectly axis-aligned rays - exactly where/how its fixed small gradient set is most
structured - and Ridged's `abs()`-fold turns that structure into sharp, high-contrast creases instead
of hiding it in a smooth gradient. Fixed by biasing the hash's input by `+0.5` before the `sin()` call,
so it's never an exact multiple of pi for any integer seed, `0` included.

**Plate Tectonics mountains didn't actually form, at any seed or simulated duration, until this fix:**
asked directly to confirm mountains "actually generate, are visible and useful," a seed sweep across
Plate Tectonics (several seeds, 0/50/100/300 Myr simulated) showed the baked height range capped
around 550-580m regardless - essentially unchanged by `MountainHeightMeters` (3000-4000m configured)
or by how long the simulation ran. Root cause, confirmed via direct instrumentation of the live
`Closure` values: `BakePlateTectonics`/`UpdateBoundaryStress` classify a plate boundary convergent/
divergent/transform by comparing `Closure` (the two plates' instantaneous drift, each projected onto
the boundary axis and subtracted) against a hardcoded `constexpr double TransformThreshold = 0.25`
(same value, same hardcoding, in the real UE plugin's `SolarOrbzTerrainLayers.cpp` - this is a latent
bug in the ported logic, not something the port introduced). `GetInstantaneousDrift()` is
`Cross(RotationAxis, SeedDirection) * AngularRateRadPerMyr`, and at the default
`InitialDriftSpeedDegreesPerMyr = 2.0`, `AngularRateRadPerMyr` tops out around 0.035 rad/Myr - so
`Closure` itself is bounded to roughly +-0.07 at the extreme, and in practice samples clustered around
0.005-0.03 in the instrumented runs. A threshold of `0.25` is simply unreachable at that scale: every
boundary read as "transform" (zero height contribution) no matter what `MountainHeightMeters` was set
to or how many simulation steps ran - and since classification, not simulated time, gates the
`AccumulatedConvergenceMyr`/`AccumulatedDivergenceMyr` counters too, this silently disabled the stress
multiplier (`MaxStressMultiplier`) AND plate splitting-at-rifts (`PlateSplitThresholdMyr`) as well -
three "done" features with no actual effect. Fixed by replacing the hardcoded constant with
`ContinentLayer::ComputeTransformThreshold()`, scaled to the plates' own configured drift speed
(`0.3 * DegreesToRadians(InitialDriftSpeedDegreesPerMyr)`) instead of a disconnected absolute value, so
it stays meaningful regardless of that speed. Verified via the same seed sweep: height range now
reaches roughly -8000m to +2500..5300m depending on seed (previously capped near +/-550m), a visible
"Snow/Ice" biome now appears in `full_pipeline.json`'s output where it never could before, and -
importantly - mountains are visible on the very first bake (`Simulated age: 0 Myr`), since
classification only needs the plates' current instantaneous drift, not accumulated simulated time;
"Run Simulation" still matters for the stress-multiplier bonus and plate splitting, just no longer for
whether mountains exist at all. Confirmed in the GUI too (Xvfb screenshot, Plate Tectonics, default
Seed 0): a real, continuous, snow-capped mountain range along the plate boundary, immediately on
selecting the algorithm. The real UE plugin (`Source/SolarOrbz/Private/SolarOrbzTerrainLayers.cpp`,
`Source/SolarOrbz/Public/SolarOrbzTerrainLayers.h`) had the identical hardcoded `0.25` at both of its
own call sites and was presumably affected the same way - since fixed there too (same
`ComputeTransformThreshold()` approach; see ROADMAP.md's own entry for that pass, including the usual
caveat for this file: not compiled or run in this environment, matched by hand against its existing
`float`-throughout convention and the already-verified standalone fix).

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
