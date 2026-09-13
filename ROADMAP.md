# SolarOrbz Roadmap

A running backlog of pending work, surfaced during design discussion but not yet built. Not a
commitment or a schedule - just a place these don't get lost between sessions.

## Terrain

- **More noise generation variety for better mountains, plains, and ravines.** Today's noise
  layers (`Noise Layer`, `Planetary Noise Layer`) are a single fractal Perlin sum - good for
  general roughness and continent-scale shape, but it doesn't give sharp mountain ridgelines, flat
  plains/plateaus, or carved ravines/canyons as distinct, controllable features. Worth adding:
  - **Ridged noise** (e.g. `1 - abs(noise)`, optionally squared/sharpened) for actual mountain
    ridgelines instead of smooth rolling hills.
  - **Terraced/plateau noise** (quantizing the height into discrete steps, optionally with a
    smoothing pass at the step edges) for flat plains and mesa-like plateaus.
  - **Billow/ravine noise** (`abs(noise)` inverted, or a dedicated flow-carving pass akin to
    Erosion's hydraulic model but tuned for sharper, narrower canyons rather than broad drainage)
    for ravines and canyons as a distinct feature rather than an erosion side effect.
  - Likely shape: either new concrete layer types alongside the existing Noise/Planetary Noise
    layers (sharing `FractalNoiseTerrainLayerBase` where it makes sense), or a "noise type" enum on
    the existing layers if the underlying math is similar enough to not warrant new classes.

- **Continent Layer** (discussed, not yet built). A whole-surface-bake layer (architecturally like
  Erosion) that places a controllable number of discrete landmasses via seeded/grown regions
  instead of noise-derived coastlines, so `USolarOrbzPlanetProfile::GetNumContinents()` /
  `GetNumIslands()` / `HasNorth/SouthPolarContinent()` - already wired into the Profile data model -
  have something reading them. This is the biggest single piece of unbuilt terrain work.

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
