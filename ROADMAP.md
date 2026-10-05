# SolarOrbz Roadmap

A running backlog of pending work, surfaced during design discussion but not yet built. Not a
commitment or a schedule - just a place these don't get lost between sessions.

## Design Principles

- **This project is planet-focused.** Star and Asteroid Profiles exist as data (from the earlier
  "modular across body types" work) but terrain generation itself - Noise, Planetary Noise,
  Continent, Canyon, Erosion, Terrace - is designed and tuned for planets. No layer carries special-
  cased logic to also suit asteroids or stars; that scope was deliberately dropped.
- **Radius Meters is an average/reference radius, not a floor.** `Elevation = 0` means "exactly at
  the actor's Radius Meters" - it is not a minimum. Every terrain layer must be able to produce
  both positive (above) and negative (below) elevation with no artificial floor or ceiling beyond
  whatever that specific layer's own authored parameters impose (e.g. Continent's Ocean Floor
  Depth is an intentional authored limit, not a hidden engine clamp). Verified with a full audit of
  every `Clamp` call in the terrain pipeline - none restrict height magnitude, only array indices,
  `Acos` domain safety, and 0-1 shape/falloff parameters.
- **Sea Level is the real reference point for "above/below water," not raw Elevation 0.** Any layer
  that defines an absolute "this many meters above/below X" position - `Noise Layer`, `Planetary
  Noise Layer`, `Continent Layer` - measures from Sea Level (`ClimateSimulation.SeaLevel`), not the
  raw base radius, so moving Sea Level moves all of their output with it, the same principle already
  applied to Climate Masks' Elevation range. `Canyon Layer` is the one exception, and it's by
  design, not oversight - it carves relative to whatever terrain is already there, not an absolute
  reference, so Sea Level doesn't apply to it.
  - Mechanism: `USolarOrbzTerrainLayer::ApplyPlanetaryContext(Profile, SeaLevelCm)`, called once per
    regenerate by the actor before `PrepareLayers()`/`Bake()`, forwarded through
    `USolarOrbzTerrainLayerStack`. Supersedes the old `ApplyProfile()` (Profile-only) - same
    non-mutating-the-shared-asset pattern, just carrying Sea Level alongside Profile now.

## Terrain

- **Terrain Graph Editor, Phase 1 (data model) + Phase 2 (editor UI).** **Done.** Requested
  directly: a dedicated node-graph UI for authoring a planet's terrain recipe instead of editing
  `USolarOrbzTerrainLayerStack::Layers` as a raw `Instanced` array in the generic Details panel. Full
  design in `Docs/SolarOrbzTerrainGraphEditor.md` (mirrored as an Artifact per `CLAUDE.md`'s
  subsystem-doc policy); the short version: the graph is a transient VIEW over `Layers`, never a
  second source of truth - `EvaluateHeight`/`Bake`/every existing caller still only ever reads
  `Layers`, completely unchanged.
  - **Phase 1 (data model)**: new file `SolarOrbzTerrainGraph.h`/`.cpp` plus small additions to
    `SolarOrbzTerrainLayers.h`/`.cpp`: `FGuid USolarOrbzTerrainLayer::EditorNodeId` (+
    `EnsureEditorNodeId()`), `TMap<FGuid, FVector2D> USolarOrbzTerrainLayerStack::EditorNodePositions`
    + a `Transient` `TerrainGraph` field + `GetOrCreateTerrainGraph()`, and the three new graph
    classes themselves (`USolarOrbzTerrainGraphNode`/`Schema`/`Graph`). No Build.cs change needed for
    this part - `UEdGraph`/`UEdGraphNode`/`UEdGraphSchema` live in the `Engine` module, already a
    dependency.
  - **Phase 2 (editor UI)**: new file `SolarOrbzTerrainGraphEditorToolkit.h`/`.cpp`:
    `USolarOrbzTerrainLayerStackAssetDefinition : UAssetDefinitionDefault` (the modern UE5.8
    registration point, replacing the older `IAssetTypeActions` pattern) makes double-clicking a
    stack asset open `FSolarOrbzTerrainGraphEditorToolkit : FAssetEditorToolkit` - an `SGraphEditor`
    tab bound to the Phase 1 graph, docked next to a `IDetailsView` tab that shows whichever node is
    selected (free, since a node references the real layer object directly). Added `GraphEditor` and
    `AssetDefinition` to `SolarOrbz.Build.cs`'s private dependencies for this. "Add Layer" is a
    toolbar combo button rather than a native graph right-click menu - `SGraphEditor`'s
    `OnCreateActionMenu` delegate's exact signature couldn't be confirmed against documented UE5.8
    sources, so the combo button (same reflection-over-subclasses approach, zero per-layer-type code)
    reaches the same outcome through APIs already proven correct elsewhere in this codebase. Also
    added `USolarOrbzTerrainGraph::IsRebuilding()` (a small Phase 1 follow-up): wiring
    `CompileToLayers()` to fire on every graph edit meant guarding against `RebuildFromLayers()`'s own
    programmatic node adds/removes re-triggering that same compile mid-rebuild and writing a
    truncated chain back into `Layers`.
  - **Logging**: every operation in both files now logs through one new category,
    `LogSolarOrbzTerrainGraph` - rebuild/compile start-finish and layer counts, connection
    allow/disallow/replace decisions, and the toolkit's open/close/add-layer/selection/compile steps -
    specifically so a real compile/run pass has a trail to debug from if any guessed API shape below
    turns out wrong, rather than a silent failure.
  - Written and reviewed against documented UE5.8 `EdGraph`/`GraphEditor`/`AssetDefinition`/
    `UnrealEd` APIs (verified via web search, not assumed from memory), not compiled or run - the same
    review-only verification bar as the chunked-terrain actor/manager code, since there is no
    Python-equivalent ground truth for UObject/Slate-adjacent graph/toolkit machinery.

- **Heightmap/Stamp layers silently contributing zero height - a real bug, found once hands-on
  testing was actually possible.** **Done.** Reported symptom: a Heightmap Layer showed no visible
  terrain at Earth radius even with extreme `MinHeightMeters`/`MaxHeightMeters` values. Root cause:
  `FSolarOrbzTextureHeightSampler::EnsureDecoded` (`SolarOrbzIcoSphere.cpp`, shared by both
  `USolarOrbzHeightmapTerrainLayer` and `USolarOrbzStampTerrainLayer`) called
  `FTextureSource::GetMipData(RawData, 0)` - a 2-arg call that only resolves against an old 3-arg
  overload (`OutMipData, MipIndex, IImageWrapperModule* = nullptr`) which does NOT decompress
  Source data stored PNG/JPEG-compressed on disk, a common default state for an imported texture -
  it silently returns `false` for exactly that case. `EnsureDecoded` returning `false` leaves
  `CachedWidth`/`CachedHeight` at 0, and `USolarOrbzHeightmapTerrainLayer::GetRawHeight` bails out
  to a hardcoded `0.0f` the moment `EnsureDecoded` fails - **before `MinHeightMeters`/
  `MaxHeightMeters` are ever read** - so the layer contributed exactly zero height regardless of how
  extreme those settings were, matching the reported symptom precisely. This was written and
  self-flagged as a risk (`// NOTE: the exact FTextureSource::GetMipData overload has shifted
  across engine versions... adjust if 5.8's header differs`) back when none of this subsystem could
  be compiled or run - exactly the kind of thing that discipline predicted would surface on first
  real use, not earlier.
  - Fix: switched to `FTextureSource::GetMipImage(FImage&, BlockIndex, LayerIndex, MipIndex)` -
    Epic's own documented modern replacement for `GetMipData`, which decompresses Source data
    internally - followed by `FImage::CopyTo(..., ERawImageFormat::R32F, RawImage.GammaSpace)` to
    normalize any source pixel format (not just the 4 the old hand-rolled switch handled) into a
    plain `float` array, preserving the source image's own gamma space (not forcing Linear) so an
    already-correctly-imported heightmap (sRGB unchecked, the normal setting for non-color data)
    round-trips through this conversion with the same numeric values as before - fixes the actual
    decode bug without silently changing how an existing heightmap's pixel values map to height.
    Added `ImageCore` to `SolarOrbz.Build.cs`'s private dependencies for `FImage`/`ERawImageFormat`.
  - Both confirmed via web search against current UE5.8-era Epic documentation (`GetMipImage`'s and
    `FImage::CopyTo`'s exact signatures), not assumed from memory - per this project's UE5.8-specific
    documentation standard, and because this exact kind of unverified-API-shape mistake is what
    caused the bug being fixed.

- **Sea Level wiring for Noise, Planetary Noise, and Continent.** **Done.** Fixed a real gap:
  Planetary Noise Layer's doc comments already claimed to be sea-level-relative, but the code never
  actually read `ClimateSimulation.SeaLevel` - it only ever measured from the raw base radius,
  silently wrong whenever Sea Level was non-zero. Continent Layer's Ocean Floor Depth/Land Plateau
  Height had the same gap. All three (plus `Noise Layer`, brought in line with the other two once
  the asteroid-specific exemption was dropped - see Design Principles) now genuinely offset by Sea
  Level via `ApplyPlanetaryContext`.

- **Noise amplitude compensation.** **Done.** `bCompensateAmplitude` (default true) on
  `FractalNoiseTerrainLayerBase` - affects Noise/Planetary Noise/Canyon Layer alike. Root cause:
  multi-octave fractal noise's typical output is naturally much smaller than its nominal peak
  (confirmed empirically - at the default 5 octaves, Perlin's typical variation is only ~17% of its
  theoretical max), which is why `Weight` often needed cranking to 10-15+ to see appreciable
  terrain. Now calibrated once per regenerate via Monte Carlo sampling (256 random points) of each
  layer's own exact Seed/Octaves/Persistence/Lacunarity/NoiseType combination, rescaling so the
  standard deviation lands on a fixed target regardless of octave count - not a fixed formula, so it
  stays accurate no matter how those are tuned. Logged under `LogSolarOrbzNoise`.
  - This also turned out to explain the "Continent Layer covers the entire planet" report from the
    same session: Continent's own influence/falloff math checked out fine on its own (verified
    numerically - default parameters land around ~4% coverage), but a Noise/Planetary Noise layer
    stacked on top with Weight cranked to 10-15 (the workaround for the amplitude issue above) was
    swinging large enough to push ocean floor back above sea level almost everywhere once combined.
    No separate Continent-specific fix needed - retest with Weight back at 1 now that amplitude
    compensation exists.

- ~~More noise generation variety for better mountains, plains, and ravines.~~ **Done.**
  - `Noise Type` (Perlin / Ridged / Billow / Value / Voronoi) on `Noise Layer` and `Planetary Noise
    Layer` - a selectable basis function per octave, not just a scale change.
  - `Terrace Layer` - quantizes height into flat plateaus with a soft-edged ramp between them
    (`Step Height`, `Edge Softness`, `Terrace Strength`, plus `Irregularity` for jittered/non-uniform
    boundaries). Matches World Creator's Terrace filter family (Simple/Steep are just different
    Step Height values; Irregular is Irregularity > 0).
  - `Canyon Layer` - narrow, sharp-edged carved grooves following the selected Noise Type's ridge
    pattern (defaults to Ridged). Built on the same fractal base as Noise Layer, so it inherits
    Seed/Octaves/Frequency/Lacunarity/Persistence/Warp for free.
  - Still open from the original World Creator filter list, lower priority (need real neighbor/
    whole-grid access, more work than the above): Smooth, Denoise, Kuwahara, Distortion, Inflate/
    Deflate/Balloon. Also skipped as out of scope/overlapping other systems: Scatter (overlaps PCG
    biome scatter), Shore (overlaps Climate sea level/moisture), the Sediment filter category
    (overlaps Erosion's existing hydraulic pass), and purely stylized effects (Hexagons, Blocks, Swirl).

- **Continent Layer.** **Done.** `USolarOrbzContinentTerrainLayer` - a pure per-point function (like
  Noise/Stamp, not a whole-surface bake in the Erosion/Terrace sense) that places a controllable
  number of continents and islands as seeded, grown landmasses with noise-perturbed coastlines,
  plus optional landmasses pinned exactly to either pole. Islands are just smaller-radius versions
  of the same seed mechanism as continents (`Min/MaxContinentRadiusDegrees` vs `Min/MaxIslandRadiusDegrees`
  independently authored), not a post-hoc size classification.
  - **Now wired to `USolarOrbzPlanetProfile`.** New `bOverrideFromProfile` toggle (default true) -
    when a Planet Profile is assigned to the actor, `NumContinents`/`NumIslands`/pole flags resolve
    from `GetNumContinents()`/etc each regenerate instead of this layer's own authored values, which
    become the fallback (used whenever no override is in effect - Profile unset, not a Planet
    Profile, `ApplyProfile()` never called, or the toggle is off).
  - This introduced a new `ApplyProfile()` virtual on the `USolarOrbzTerrainLayer` base class and a
    matching forwarder on `USolarOrbzTerrainLayerStack`, called by the actor once per regenerate,
    before `PrepareLayers()`/`Bake()` - any future layer that wants Profile data can hook into this
    the same way. Deliberately does NOT mutate the layer's own UPROPERTY fields when overriding
    (would corrupt a shared/reused-across-planets asset's saved data) - the resolved values are
    cached in a private `TWeakObjectPtr`, resolved fresh inside `Bake()` each time.
  - Known simplification: seed placement is pure uniform-random on the sphere, not blue-noise/
    Poisson-disc, so seeds can occasionally cluster closer together than a hand-placed layout would.
  - **Continents/islands no longer overlap an enabled polar continent.** `Bake()` now
    rejection-samples each random continent/island's position+radius (re-rolling together, up to a
    32-attempt cap) against both pole flags, accepting only a placement whose angular distance from
    an enabled pole is at least that pole's `PolarContinentRadiusDegrees` plus the new seed's own
    radius - i.e. the two landmasses don't actually overlap. Doesn't protect seeds from overlapping
    each other, only from colliding with the poles' fixed, author-placed landmasses. A no-op (zero
    extra `FRandomStream` draws) whenever neither pole is enabled, so existing continent layouts
    seeded without poles are unaffected.
  - **"Continents still look too circular" → three selectable algorithms.** **Done.** Researched
    real procedural-generation references (not guessed) before implementing - Red Blob Games'
    island-shaping article, Azgaar's Fantasy Map Generator, Inigo Quilez's domain-warping article,
    and Cortial/Peytavie/Galin/Guerin's "Procedural Tectonic Planets" (CGF 38:2, 2019) - then added
    a new `Algorithm` property (`ESolarOrbzContinentAlgorithm`) picking between them per-instance:
    - **Radial Seeds** (the original approach, enhanced, still the default): the circle test
      survives, but the SAMPLE POINT is now domain-warped before that test runs (`CoastlineWarpStrength`/
      `CoastlineWarpFrequency`, the same warp-vector construction `USolarOrbzFractalNoiseTerrainLayerBase`
      already uses for its own `WarpStrength`, applied here to geometry instead of to a noise
      function's own sample position) - this is the fix that actually matters, since perturbing the
      output radius (the old and only mechanism) can never change the boundary's topology, only its
      wobble. Also upgraded the boundary wobble from one `PerlinNoise3D` call to proper multi-octave
      fBm (`CoastlineNoiseOctaves`/`Persistence`/`Lacunarity`, reusing `SolarOrbzNoiseBasis::SampleBasis`
      from the Noise layer rather than a second implementation), and added metaball sub-seeds
      (`SubSeedsPerLandmass`, scattered within `SubSeedScatterFraction` of each parent's radius) -
      the union of several overlapping circles reads far less like one circle, and costs nothing
      extra in `GetRawHeight` since it already takes the max influence across every seed.
    - **Voronoi Growth**: each landmass starts at one cell of a new bake grid
      (`BakeGridWidth`/`Height`, same `FSolarOrbzLatLongGrid` pattern Erosion/Terrace already use)
      and randomly floods into unclaimed neighbor cells, decaying a growth-energy budget by a
      randomized factor (`GrowthDecayMin`/`Max` plus `GrowthJitter`) each hop - picking a *random*
      cell from the current frontier each step, not strict same-decay BFS rings, is what actually
      breaks radial symmetry into real bays/peninsulas. `MinContinentRadiusDegrees`/etc are reused
      (converted into an equivalent hop budget via the grid's own degrees-per-cell) so both
      algorithms stay authored in the same units. A cell can only be claimed once, so overlap
      between landmasses - poles included, which claim their cap first - is prevented by
      construction instead of Radial Seeds' rejection-sampling.
    - **Plate Tectonics**: approximate, explicitly NOT a physics simulation (same framing the paper
      itself uses) - partitions the sphere into `NumPlates` randomly-drifting plates via
      nearest-seed/Worley assignment, classifies each plate-boundary cell as convergent/divergent/
      transform from the two plates' relative drift, then a multi-source BFS (`BoundaryInfluenceDegrees`
      cutoff) carries each boundary's classified modifier (`MountainHeightMeters`/`TrenchDepthMeters`/
      `RidgeHeightMeters`/`RiftDepthMeters`) outward with a falloff, so a boundary reads as an actual
      mountain range/trench/ridge/rift, not a one-cell seam. `NumContinents`/`NumIslands` do nothing
      in this mode - continents are an emergent side effect of `OceanicPlateFraction`, not an
      authored count. The pole flags still work the same way as the other two algorithms (force the
      polar cap's plate assignment continental via a sentinel id), so that one feature behaves
      identically regardless of which algorithm is active.
    - Voronoi Growth and Plate Tectonics share one bake grid and one cached height raster
      (`BakedHeightCm`, sampled bilinearly via `FSolarOrbzLatLongGrid::SampleBilinear`, same
      whole-surface-bake-then-sample shape Erosion/Terrace use); Radial Seeds alone stays a pure
      per-point function with no grid, unchanged in kind from the original design.
    - Switching `Algorithm` re-rolls the whole layout, the same as changing `Seed` - no attempt at
      cross-algorithm layout compatibility.

- **Erosion Rainfall Amount** is uniform across the planet by default, not yet driven by a Climate
  Simulation's actual computed moisture. Wiring the two together would let erosion carve more
  aggressively in wet regions and barely at all in deserts, instead of uniformly everywhere.

- **"Terrain not moving vertices enough at Earth scale to be noticeable."** **Done (partially -
  see below).** Root cause was never the height math - `Height` is computed correctly, in real
  centimeters, same as always. It's mesh resolution: `Max Subdivisions` is a hard cap independent
  of `Radius Meters`, so at Earth's radius (~6.37M meters) the subdivision level actually needed to
  hit a reasonable `Vertices Per Meter` (>20) is unreachable, and the cap silently binds - default
  settings gave ~104km triangle edges, geometrically incapable of showing meter-to-km-scale terrain
  regardless of how tall it actually is.
  - Fixed: `RadiusMeters`/`RadiusCm` and the whole icosphere generator's radius parameter are now
    `double`, not `float` - at Earth scale, float's ~7 significant digits was already losing tens of
    centimeters before any terrain math ran, independent of the resolution issue above.
  - Fixed: `Max Subdivisions`' editable range widened (`ClampMax` 6 -> 11 - level 11 is ~42M
    vertices, a reasonable one-off-bake ceiling; still capped hard since this actor recomputes its
    whole mesh live on every property change, and going higher risks freezing/crashing the editor
    rather than just being slow). Tooltips on `Radius Meters`/`Vertices Per Meter`/`Max Subdivisions`
    now spell out the edge-length math. `RegenerateMesh` now distinguishes two warning tiers: the cap
    is binding but raising it would help, vs. the requested density (`EstimateVertexCount` in the
    billions) is infeasible for any single mesh regardless of `Max Subdivisions`.
  - **Not fixed, deliberately out of scope for this pass:** a single uniform-subdivision mesh
    fundamentally cannot show ground-level detail at planetary radius no matter how high `Max
    Subdivisions` goes - that needs a chunked/streaming LOD terrain system (camera-distance-adaptive
    patches), which is planned separately on a Nanite-based custom backend, not a bigger single mesh.
    This actor remains the right tool for a bounded preview/bake radius or zoomed-in testing.
  - **Still true after the perf pass directly below** - parallelizing the per-vertex evaluation
    makes a given subdivision level regenerate faster, it doesn't change what a single mesh can
    geometrically represent. Nothing here moves until the chunked/streaming system above exists.

- **Regenerate performance / "the whole function" pass.** **Done**, prompted directly by dissatisfaction
  with how `RegenerateMesh` felt to iterate against, independent of any one specific bug. Three changes,
  all in `ASolarOrbzIcoSphereActor`/`USolarOrbzTerrainLayerStack`:
  - **The two expensive per-vertex loops (base terrain Pass A, biome Pass B in `RegenerateMesh`) are now
    `ParallelFor`'d** instead of single-threaded `for` loops - this is the dominant cost at any real
    vertex count, including every Earth-scale/high-`Max Subdivisions` case above, so it's the single
    biggest lever for both interactive editing and bake time. Safe to do now specifically because the
    rest of the codebase was already written defensively for it: every mask/noise evaluation path
    (`USolarOrbzBiomeMask` subclasses, `USolarOrbzTerrainLayerStack::EstimateSlopeUpTo`) already used
    `thread_local` recursion guards rather than shared mutable counters, and all per-regenerate setup
    (`ApplyPlanetaryContext`/`PrepareLayers`/`Bake`) already ran once, single-threaded, before any
    per-vertex evaluation - nothing architectural had to change to make this safe, it just hadn't been
    switched on. One real gap found and fixed to make it actually safe:
    `USolarOrbzHeightmapTerrainLayer`/`USolarOrbzStampTerrainLayer` lazily decoded their texture
    (`FSolarOrbzTextureHeightSampler::EnsureDecoded`) from inside `GetRawHeight` itself - harmless
    single-threaded, but a real first-touch data race once `GetRawHeight` is called from multiple
    worker threads at once. Fixed via a new `USolarOrbzTerrainLayer::WarmCaches()` virtual, called
    unconditionally (every enabled layer, not just `RequiresWholeSurfaceBake()` ones) from
    `PrepareLayers()` - single-threaded, before the parallel passes start.
  - Pass A's and Pass B's log-only Min/Max/Sum/count stats no longer accumulate into shared locals
    from inside the parallel section (a real data race under `ParallelFor` - it has no built-in
    reduction) - Pass A writes a per-vertex `Heights` scratch array and reduces it in a cheap
    sequential pass afterward; Pass B's two hit-counters are `FThreadSafeCounter` instead of `int32`.
    Pass B's small per-vertex scratch arrays (`LayerWeights`/`TopBiomeIndices`/`TopBiomeWeights`) moved
    from being hoisted above the loop (reused in place - fine single-threaded, but concurrent tasks
    would corrupt each other's data in the same arrays) to declared fresh inside each iteration -
    reintroduces a small per-vertex heap allocation, an accepted, clearly cheaper-than-the-alternative
    trade given what's being parallelized around it.
  - **`PostEditChangeProperty` no longer re-runs the entire pipeline for a purely cosmetic edit.**
    `DefaultMaterial`/`BiomeBlendMaterial` were in the regenerate-trigger list alongside `RadiusMeters`/
    `TerrainStack`/etc., even though neither affects geometry at all - swapping which material is
    assigned, or which exact Biome Blend Material asset is referenced while blend mode is already live,
    is now just `ProcMesh->SetMaterial()` (plus updating the MID's texture param for the blend case),
    no icosphere rebuild/terrain/climate/biome re-evaluation. Deliberately conservative about when this
    fast path applies (`ASolarOrbzIcoSphereActor::TryApplyCosmeticOnlyChange`) - `bShowBiomeDebugColors`
    toggling and `DebugBiomeMaterial` changes still fall through to a full regenerate, since
    `BiomeDebugColors` (unlike the blend arrays) isn't cached across calls today; turning blend mode on
    for the first time also still needs a real regenerate (nothing cached yet to reuse). Correct in
    both cases, just not maximally fast - safe default over a riskier guess at correctness no compiler
    was available to check.
  - **Not done, flagged rather than silently skipped:** `RecomputeSmoothNormals` (its own full
    triangle-walk accumulation pass, called up to twice per regenerate) is still single-threaded - its
    write pattern (multiple triangles scatter-adding into shared per-vertex normal accumulators) isn't
    the same embarrassingly-parallel shape as Pass A/B's one-task-per-vertex loops, so parallelizing it
    safely needs either per-task partial accumulators merged afterward or an atomic-add approach,
    neither implemented here. Worth a follow-up pass if it shows up as a real bottleneck once the two
    bigger loops above are no longer the dominant cost.

## Climate / Biome

- **Per-layer terrain masking.** **Done.** Any layer in the main `USolarOrbzTerrainLayerStack` can
  now optionally carry its own procedural mask (`USolarOrbzTerrainLayer::Mask`, the same Climate
  Mask/Composite Mask types `USolarOrbzBiome::Mask` already uses), scoping that individual layer's
  contribution the way World Creator's per-filter masks do - "Erosion only in wet biomes," "Noise
  only above a latitude band" - directly in the primary stack, with real blend-mode control and
  explicit ordering relative to every other layer, rather than through a separate whole-biome pass
  after the fact. A Composite Mask here can reference an existing Biome asset to reuse its condition
  without redefining it. `USolarOrbzBiome::TerrainDetail` (the older additive secondary-stack
  mechanism) is kept and unchanged for existing planets and for a self-contained biome-only pass,
  but is no longer the first reach for new terrain-shaping work - masked main-stack layers are.
  `USolarOrbzBiomeStack`'s whole-biome-identity role (ground texturing, debug colors, future PCG
  scatter) is unaffected and remains the classification authority per the PCG Biome Core note below.
  - Masks that read Temperature/Moisture (`USolarOrbzBiomeMask::NeedsClimateData`) need a real
    Climate Simulation grid to be meaningful, which doesn't exist until after the base terrain pass
    has already run once. Handled by a cheap `AnyLayerNeedsClimateData` check: when no layer's mask
    needs it (the common case, and every planet authored before this existed), the base terrain pass
    still runs exactly once, same cost as always. Only when a layer's mask actually needs it does the
    pass re-run a second time after Climate Simulation, so that mask sees real data instead of its
    no-simulation fallback. Masks that read Slope (`NeedsSlope`) get a finite-difference estimate
    mid-pass (no real mesh normal exists yet) - a known simplification (single-tangent-direction, not
    a true 2-axis gradient), same spirit as Erosion's own equatorial-only cell spacing approximation.
  - Known limitation, inherited from the existing one-bake-per-regenerate design: a whole-surface-
    baked layer (Erosion, Terrace) only ever sees the seed-pass (climate-neutral) result of any masked
    layer below it in the stack, never the final climate-aware one, since `Bake()` runs once, before
    either terrain pass walk.
  - Also fixed in the same pass: the blend-mode-as-first-enabled-layer footgun (`EvaluateHeightUpTo`
    used to start `Accum` at 0.0, so a first layer set to Multiply always yielded 0 forever, Min/Max
    silently floored/clipped everything to 0, and Subtract silently negated the layer's own output) -
    the first enabled layer in a stack is now always treated as an implicit Replace, matching how
    every other layer-stack tool treats a stack's bottom filter. Intentional behavior change: a
    planet whose first enabled layer relied on the old Subtract-from-zero quirk for a negative base
    will see it flip sign - author that with `Weight = -1` on an Add/Replace layer instead.
  - Each `USolarOrbzBiome::TerrainDetail` stack now also gets `ApplyPlanetaryContext`/`PrepareLayers`
    called once per regenerate (previously never called for these secondary stacks at all) - fixes a
    real pre-existing gap where a whole-surface-baked layer (Erosion, Terrace) inside `TerrainDetail`
    silently contributed nothing (never baked), and Sea-Level-relative layers measured from 0 instead
    of the real Sea Level. Also what lets a `Mask` on a `TerrainDetail` layer read Slope correctly.

- **Terrain/climate pipeline cleanup**, found during the same review as the two items above.
  **Done.** `USolarOrbzBiomeStack::EvaluateBiomeTerrainContribution`/`GetDominantBiome`/
  `EvaluateTopWeightedBiomes` used to each independently re-walk `Layers` and re-evaluate every
  mask from scratch when called back-to-back per vertex in `RegenerateMesh` Pass B - up to 2x the
  mask evaluations needed, worse for recursive Composite Masks. Now share one `EvaluateLayerWeights`
  result per vertex (new precomputed-weights overloads; original signatures kept as thin wrappers).
  `FSolarOrbzMaskRange::Evaluate` no longer silently returns 0 everywhere if an author leaves
  `Min > Max`. The fractal noise seed hash (`ComputeNormalizedNoiseUncompensated`'s `SeedOffset`,
  depends only on `Seed`) is now cached once per `Bake()` instead of recomputed on every vertex, and
  shares one formula (`SolarOrbzNoiseBasis::ComputeSeedOffset`) with Terrace's identical
  `IrregularitySeedOffset` instead of duplicating it. The equirectangular lat/long grid walk +
  longitude-wrapping bilinear sample - independently reimplemented across Erosion, Terrace,
  Continent's coverage sampler, and Climate Simulation's grid/`Sample()` - is now one shared
  `FSolarOrbzLatLongGrid` utility (`Source/SolarOrbz/Public/SolarOrbzLatLongGrid.h`).

- **PCG Biome Core integration** for content scattering (trees, rocks, and eventually
  civilization - roads, farms, settlements). SolarOrbz's Climate + BiomeStack system stays the
  classification authority ("what biome is here"); PCG Biome Core would consume that as its
  texture-driven biome input and handle the actual scattering, which it's already built for. Likely
  bridge: bake biome classification out as an equirectangular "Biome ID" texture, same convention
  already used for the Climate and Erosion grids. PCG Biome Core is still Experimental as of UE 5.5
  - acceptable since this project is pinned to a single engine version (5.8), but worth remembering
    if that ever changes.

- **Atmosphere composition randomization** on `USolarOrbzProceduralPlanetProfile` is a stub -
  `GetAtmosphereComposition()` always returns empty for procedural profiles. Density and pressure
  are randomized; gas mixtures are not yet.

## Planetary / World

- **SolarOrbz toolbar button/Window menu entry intermittently showing ASNMechLab's instead - a
  real registration collision, found via hands-on use.** **Done.** Reported symptom: reopening the
  editor sometimes shows the MechLab Loadout Validator when clicking what the user expects to be the
  SolarOrbz button/menu entry, and which plugin "wins" flips between sessions. Root cause, confirmed
  by reading both plugins' editor modules side by side (`SolarOrbzEditor.cpp` here,
  `ASNMechLabEditor.cpp` in the sibling `all-systems-nominal` repo's `Plugins/ASNMechLab`): both were
  built from Epic's Editor Standalone Window plugin template and neither renamed two template
  defaults that only matter when two such plugins coexist in one project. First, both left the
  template's own `PlaceholderButtonIcon.svg` unmodified, so the two toolbar buttons were visually
  identical - fixed previously by giving SolarOrbz its own icon
  (`Resources/SolarOrbzButtonIcon.svg`). Second, and the actual cause of entries *disappearing*
  rather than just looking alike: both plugins' `UI_COMMAND(OpenPluginWindow, ...)` macro stringifies
  the literal C++ identifier `OpenPluginWindow` as the command's internal name - the `TCommands`
  `BindingContext` ("SolarOrbz" vs "ASNMechLabEditor") only scopes the keybinding editor's display,
  not the `FToolMenuEntry` Name used inside a shared menu section. Both plugins register their
  button into the identical `LevelEditor.LevelEditorToolBar.PlayToolBar` → `PluginTools` section and
  their Window-menu item into the identical `LevelEditor.MainMenu.Window` → `WindowLayout` section,
  so both entries landed on the same implicit Name (`"OpenPluginWindow"`) in the same section -
  whichever module's `RegisterMenus()` ran last (module load order, not stable across editor
  sessions) was the one left visible there, in the toolbar and the Window menu simultaneously,
  matching the reported symptom exactly.
  - Fix: `FSolarOrbzModule::RegisterMenus()` now passes an explicit, project-unique
    `FName("SolarOrbz_OpenPluginWindow")` as the entry Name at both registration sites - the
    toolbar button (set directly on the `FToolMenuEntry` returned by `InitToolBarButton` before
    `Section.AddEntry`) and the Window menu item (via `AddMenuEntryWithCommandList`'s
    `InNameOverride` parameter, confirmed via its real signature rather than assumed). This makes
    SolarOrbz's slot in both shared sections collision-proof regardless of what any other current or
    future plugin's own command happens to be called - it no longer matters that `ASNMechLab` (out
    of scope for this repo - lives in `all-systems-nominal`) has the identical problem on its own
    side; fixing it there would need the same treatment in its own `ASNMechLabEditor.cpp`.
  - Both the icon-identity and the entry-Name-collision facts were confirmed by directly reading
    `all-systems-nominal/Plugins/ASNMechLab/Source/ASNMechLabEditor/`'s own source, not assumed -
    and the exact `AddMenuEntryWithCommandList` signature (which parameter is the Name override, and
    where in the parameter list) was verified via web search before being used, per this project's
    UE5.8-specific documentation/verification standard.

- **Planet Spawner Graph.** **Done.** Requested directly: turn the SolarOrbz dock tab's flat
  "enter Radius, click Generate" panel into a node graph like the Terrain Graph Editor's. Full
  design in `Docs/SolarOrbzPlanetSpawnerGraph.md` (mirrored as an Artifact per `CLAUDE.md`'s
  subsystem-doc policy). Unlike the Terrain Graph Editor's chain, a planet's fixed modules (Base
  Sphere/Biome Stack/Climate Simulation/Profile) have no real order between them, so that part of
  the graph is a fixed, non-editable layout - four module nodes wired to a single "Planet"
  sentinel, not a reorderable chain. New files `SolarOrbzPlanetSpawnerGraph.h`/`.cpp` (four small
  transient `UObject` module configs + `USolarOrbzPlanetSpawnerGraphNode`/`Schema`/`Graph`,
  `LogSolarOrbzPlanetSpawner` category). Rewired `SSolarOrbzMainPanel` (`SolarOrbzEditor.h`/`.cpp`)
  to host an `SGraphEditor`+`IDetailsView` split (same selection-follows-Details pattern as the
  Terrain Graph Editor's toolkit). No Build.cs change needed - `SGraphEditor` lives in an
  already-depended-on module. Written and reviewed against documented UE5.8 APIs (verified via web
  search), not compiled or run - review-only, same bar as the rest of this plugin's editor-only
  code.

  **Pivot: CSV catalog thrown out for an embedded terrain chain.** A first pass added
  `SolarOrbzPlanetCatalogRow.h` (`FSolarOrbzPlanetCatalogRow : FTableRowBase`, importable from a
  `.csv`) plus a `USolarOrbzPlanetTerrainModule` node referencing an external
  `USolarOrbzTerrainLayerStack`. Both were removed - deleted the catalog row file entirely, deleted
  the Terrain module class - in favor of embedding the real terrain-layer authoring chain (the same
  Start→Layer→…→Output mechanism the standalone Terrain Graph Editor uses, noise layers included)
  directly into this graph's own canvas, reusing `USolarOrbzTerrainGraphNode` as-is since it's
  graph-agnostic. `USolarOrbzPlanetSpawnerGraph` gained `EmbeddedTerrainStack`/
  `EmbeddedTerrainStartNode`/`EmbeddedTerrainOutputNode` plus `RebuildEmbeddedTerrainChain()`/
  `CompileEmbeddedTerrainChain()` - duplicates of `USolarOrbzTerrainGraph`'s own
  `RebuildFromLayers()`/`CompileToLayers()`, with the one necessary difference that they filter
  `Nodes` by type before touching anything, since this graph's fixed module/Planet nodes share the
  same `Nodes` array and can't be wiped the way the standalone graph safely wipes its own. The
  schema (`CanCreateConnection`) now branches on pin category: `ModulePinCategory` stays
  disallow-always, `HeightPinCategory` gets the standalone schema's same
  single-connection/replace-on-reconnect behavior. The panel swapped its Data Table/Apply Row
  section for an "Add Layer" combo button mirroring the standalone toolkit's
  `BuildAddLayerMenu()`/`AddLayerOfClass()` exactly, and `OnGenerateClicked` now compiles the
  embedded chain and assigns it straight onto the preview actor's `TerrainStack` field. Biome Stack
  deliberately stays an external asset reference, unchanged - only Terrain had the ordered-chain
  shape worth inlining.

- **Chunked/streaming planet terrain** - the actual fix for ground-level detail at true planetary
  radius that every entry above flags as out of scope for the single-mesh `ASolarOrbzIcoSphereActor`.
  Design + first foundational piece (an icosphere-based chunk addressing/mesh generation, built
  directly on `FSolarOrbzIcoSphereGenerator`'s own base-icosahedron table and subdivision scheme -
  `FSolarOrbzChunkAddress`/`FSolarOrbzIcoSphereChunkGenerator` in `SolarOrbzIcoSphereChunk.h/.cpp`;
  superseded an earlier cube-sphere-based version, see the design doc's revision note) written, plus
  the pentagon-vertex ("5-valent") neighbor lookup for the 12 permanently-5-valent base icosahedron
  vertices (`FSolarOrbzChunkAddress::IsAnchoredAtOriginalVertex`/`GetPentagonVertexNeighbors` - checked
  independently against the real 20-face table), plus the general (valence-6) same-depth edge-neighbor
  finder (`FSolarOrbzChunkAddress::GetEdgeNeighbor` - exhaustively verified outside the engine, depths
  0-6/all 20 faces/every edge, 327,660+ cases with zero failures, against a brute-force first attempt
  that was caught wrong and discarded - see the design doc's revision notes); neither lookup is yet
  wired into any actual seam-stitching. **Streaming/residency manager, all 7 checklist items now
  built and wired together - NOT yet compiled, run, or rendered even once (no UE5.8 compiler
  available in this environment the whole way through - see the design doc's own closing caveat on
  this item).** Point-location
  (`FSolarOrbzChunkPointLocator::FindChunkContainingDirection` - point-in-spherical-triangle descent,
  verified independently twice outside the engine, 259,000+ then another 3,200+30 cases, zero
  failures) and the LOD policy (`FSolarOrbzChunkLODPolicy::ShouldSplit`/`ShouldMerge` - longest-edge/
  nearest-distance ratio with hysteresis, checked against hand-computed depth/distance numbers then
  independently re-derived, zero discrepancies) are built
  (`SolarOrbzChunkPointLocation.h/.cpp`, `SolarOrbzChunkLODPolicy.h/.cpp`), and so is the unrestricted
  residency walk (`FSolarOrbzChunkResidencyWalker::GatherDesiredLeaves` -
  `SolarOrbzChunkResidencyWalk.h/.cpp` - recursively applies the LOD policy from the 20 base faces to
  produce a naive desired leaf-set; verified twice independently outside the engine, 20,000+ coverage/
  overlap samples and 4,000+ leaves checked for nesting/`MaxDepth` compliance each time, zero
  failures), and so is the restricted-quadtree fixpoint
  (`FSolarOrbzChunkRestrictedQuadtree::ApplyNeighborDepthRestriction` -
  `SolarOrbzChunkRestrictedQuadtree.h/.cpp` - force-splits leaves that are too shallow next to a deep
  same-depth edge-neighbor via `GetEdgeNeighbor`, the actual reason that function got built first, so
  no two adjacent leaves differ by more than 1 depth level; deliberately does NOT separately restrict
  the 12 pentagon vertices' corner-fan case, a measured-not-assumed call - worst observed same-point
  spread stayed at 2 levels across adversarial tests even without a dedicated pass; verified twice
  outside the engine, 238,245+ then another 10,737+ (leaf, edge) pairs checked for the neighbor-depth
  invariant, zero failures both times, tiling re-confirmed with zero gaps/overlaps after fixing up);
  resident-chunk spawn/despawn is built too (`FSolarOrbzChunkResidentSetDiff`/
  `FSolarOrbzChunkResidentSetManager` - `SolarOrbzChunkResidentSetDiff.h/.cpp`,
  `SolarOrbzChunkResidentSetManager.h/.cpp` - a pure set-difference, verified twice independently
  (20,004 then another 15,000 fuzzed cases, zero failures both times), plus a real
  `UProceduralMeshComponent` spawn/despawn manager that can actually create and destroy chunk geometry
  for a given viewer position, with mesh generation strictly parallelized separately from game-
  thread-only component creation); skirts for seam-hiding are built too
  (`FSolarOrbzChunkSkirtBuilder::AppendSkirts` - `SolarOrbzChunkSkirtBuilder.h/.cpp` - a pure,
  additive post-process appending a thin inward-facing wall along each chunk's 3 boundary edges,
  checked across 6 Resolution values and multiple skirt depths against additivity/counts/no-
  duplication/index-validity, zero failures); and the owning actor is built too
  (`AASolarOrbzChunkedPlanetActor` - `SolarOrbzChunkedPlanetActor.h/.cpp` - parallel to, not a
  replacement for, `ASolarOrbzIcoSphereActor`; owns a repeating update timer, a
  `FSolarOrbzChunkResidentSetManager` instance, and resolves a viewer position from an assigned
  actor, an explicit Phase-1-testing override, or a well-defined center-of-planet fallback - the
  resident-set manager itself gained a `SkirtDepth` constructor parameter so skirts actually run
  before each new chunk's component is created, the one existing file this final item needed to
  touch). Every item 1-7 piece now exists and is wired together end to end, and **the plugin compiles
  clean as of `e5bfd66`** (the first real build caught one genuine bug - a declaration-order mistake
  in `FSolarOrbzPentagonVertexNeighbors`, invisible to every verification pass up to that point since
  none of them involved an actual compiler - fixed in that same commit). Compiling is NOT "known to
  work" - this has not been played/rendered even once yet; see
  `Docs/ChunkedPlanetTerrain_TestingGuide.md` for the first-playtest runbook (setup steps, a localized
  test sequence, and a troubleshooting table covering the specific risk areas - winding, skirts,
  viewer configuration, collision). Still no baking/ASN_MK1 integration yet - both remain separate,
  not-yet-started work. See
  `Docs/ChunkedPlanetTerrain.md` for the full design, the phased CPU-first-then-GPU-compute plan, and
  the explicit list of what's deliberately not built yet - don't start the GPU-compute phase before
  the CPU-chunked streaming architecture is proven out.

- **Real-time orbit and rotation.** Planets need to actually orbit their star and rotate on their
  axis in real time, with players able to seamlessly leave one planet and travel to another. This
  is a separate actor/simulation system built on top of the current terrain pipeline, not a change
  to it - noted here so it isn't lost, not scoped yet.

- **Non-spherical asteroid shapes.** `USolarOrbzAsteroidProfile::Irregularity` exists but isn't
  wired to anything - the mesh generator always displaces a true sphere. Small, irregular-looking
  bodies are approximated today via a raw (any-scale) Noise Layer with amplitude comparable to the
  body's own radius, not a genuine non-spherical base shape.

- **Star surface rendering.** `USolarOrbzStarProfile` holds Luminosity/Temperature/SpectralClass
  but nothing yet drives a star-appropriate material or surface behavior (turbulence, emissive
  look) from it.

## Documentation

- The `Docs/SolarOrbz_Reference_Guide.pdf` is stale as of the Profile system and the retrofit of
  Climate Simulation's Atmosphere Density. Update deferred on purpose until more of the above lands,
  so it's one documentation pass instead of several small ones.
