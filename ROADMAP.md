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
