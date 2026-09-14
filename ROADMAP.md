# SolarOrbz Roadmap

A running backlog of pending work, surfaced during design discussion but not yet built. Not a
commitment or a schedule - just a place these don't get lost between sessions.

## Terrain

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

## Climate / Biome

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
