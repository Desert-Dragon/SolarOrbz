# Orbit to Ground: Playable Planet Roadmap

**Purpose:** a single, dependency-ordered to-do list for turning what exists today (a mature terrain/
biome/climate authoring pipeline, plus an unplayed chunked-streaming design) into an actual game loop
a player can fly in from orbit and walk around on the ground in. This is deliberately cross-repo -
`SolarOrbz` (terrain), `all-systems-nominal`/ASN_MK1 (gravity, atmosphere, flight, player), and
`MMOBackend_Fork` (networking) all have to meet in the middle for "orbit to ground" to mean anything.

**Read this before anything else below: a load-bearing correction.** `Docs/ChunkedPlanetTerrain.md`'s
"ASN_MK1 integration" section, and several `ROADMAP.md` entries, describe `AASNPlanetActor`/
`UASNPlanetGravityComponent`/`IASNAtmosphereSource` as existing types with an established contract
("currently assume a planet is exactly one actor..."). **Checked directly against `all-systems-
nominal` (`Desert-Dragon/all-systems-nominal`, commit `184fbf7`) while building this roadmap: none of
that exists.** The repo is 3 commits deep, `Source/ASN_MK1/` is unmodified module boilerplate (no
gameplay classes at all), and `Content/` holds only `SolarOrbz/Terrains` (baked terrain assets, nothing
gameplay-related). There is no gravity component, no atmosphere interface, no player pawn/controller,
no "Mech Lab" vehicle code - despite `ROADMAP.md`'s own references to an `ASNMechLab` plugin and a
"Mech Lab" editor wiki page existing elsewhere in this project's conversation history. Whatever of
that exists today is either uncommitted locally, lives somewhere not yet attached to this session, or
was aspirational description that got written down as if it were verified fact. **Treat every "ASN_MK1
already has X" claim in any existing doc as unconfirmed until re-checked against the real repo at the
time you act on it** - this roadmap's own Stage D below starts from "nothing exists," not from "wire
up to the existing gravity component," because right now there isn't one.

This also means: this list is written from the terrain side. It cannot specify exact class names/APIs
for the gravity/flight/player layer, because that layer doesn't exist yet to name - those stages below
describe the PROBLEM each piece has to solve and the real constraints (from UE5.8's own documented
behavior and from what SolarOrbz already produces), not a pre-decided implementation.

## How to read the status tags

- **Done** - built, and hands-on verified (played/rendered/compiled at least once, not just reasoned
  through).
- **Written, unplayed** - code exists and compiles clean, but has never been run - see each item's
  own doc for the exact "verify before relying on this" caveats. Expect the first real playtest to
  surface bugs this stage's own review couldn't catch.
- **Not started** - no code, possibly not even a settled design.

---

## Stage A — Terrain & biome authoring (mostly Done)

The actual height/climate/biome generation pipeline is the most mature part of this whole roadmap.
Nothing here blocks Stage B; this section exists so the rest of the list doesn't have to re-derive
what's already solid.

- [x] **Done.** Layer-stack terrain generation (Noise, Planetary Noise, Heightmap, Stamp, Erosion,
  Terrace, Continent - Radial Seeds/Voronoi Growth/Plate Tectonics) - `SolarOrbzTerrainLayers.h/.cpp`.
- [x] **Done.** Climate simulation (temperature/moisture, wind/orographic rain shadows) and the new
  Hydrology simulation (D8 flow accumulation) - `SolarOrbzClimateSimulation.h/.cpp`,
  `SolarOrbzHydrologySimulation.h/.cpp`.
- [x] **Done.** Biome mask system (Climate/Composite/Noise/Area/Polygon/Path/BorderBlend/
  FilterChange/Rivers/Simulation masks) - `SolarOrbzBiomeSystem.h/.cpp`. 7 of World Creator's 9
  "Layer" Distribution entries now have an equivalent; Mask-paint and Model (scattering) remain
  explicitly out of scope for this hand-authored-without-a-compiler approach (see below, Stage G).
- [x] **Done.** Standalone Continent Generator (CLI + GUI) for fast iteration without a UE rebuild
  loop, including Shape Stamps, an Ocean Overlay, and UE5-import-ready exports (heightmap, biome ID,
  PCG scatter-point CSV).
- [ ] **Not started, real gap for ground-level play specifically:** none of the above has ever been
  checked for what it looks/feels like at 1:1 human scale standing on the ground - every verification
  so far is "does the math/data look right," not "does this read as interesting terrain from a
  walking character's eye height." Stage B's first playtest is also the first time this becomes
  checkable at all.

## Stage B — Chunked/streaming planet surface (Written, unplayed)

The actual mechanism that makes a real-radius planet renderable at all - `ASolarOrbzIcoSphereActor`'s
single whole-sphere mesh cannot show ground-level detail at planetary scale, full stop (see
`Docs/ChunkedPlanetTerrain.md`'s own opening section for the vertex-count math). All 7 items of
"Phase 1" are written and wired together; **hands-on testing so far has only gone as far as flying a
camera close to a generated planet in-editor (surfaced and fixed 2 real bugs - a ray-tracing BLAS
eviction `ensure()`, and a real use-after-reallocation bug in the skirt builder) - nobody has walked on
one, and collision isn't even generated for streamed chunks yet.**

1. [ ] **First real playtest pass beyond "fly near it."** Follow
   `Docs/ChunkedPlanetTerrain_TestingGuide.md` end to end; specifically check the things that doc
   flags as unverified: triangulation winding (front/back facing) on a generated chunk, skirt
   visibility at a forced 1-level LOD mismatch, and antimeridian distortion on a chunk that straddles
   the UV seam.
2. [ ] **Collision on streamed chunks.** Explicitly not built yet
   (`FSolarOrbzChunkResidentSetManager`'s own header says so) - nothing can stand on a streamed
   planet until this exists. Needs a decision: per-chunk `UBodySetup`/collision mesh generated
   alongside each chunk's render mesh (simplest, matches `ASolarOrbzIcoSphereActor`'s own
   `bEnablePreviewCollision` pattern), vs. a simplified/lower-res collision proxy per chunk (cheaper,
   standard for high-detail streamed terrain, but a second mesh to keep in sync).
3. [ ] **Per-update spawn/despawn budget.** `UpdateResidentSet` currently has no frame-time budget -
   a large viewer jump (teleport, fast orbital-to-surface transit) generates/creates every
   newly-needed chunk in one call. Directly blocks Stage F (the orbit-to-ground transition is
   exactly the "large viewer jump" case this gap warns about) - a ship dropping from orbit to the
   surface in a few seconds needs this solved, not just normal walking-speed streaming.
4. [ ] **Real vertex-stitching at LOD seams**, replacing/supplementing skirts. Skirts hide the crack;
   they don't fix it, and a skirt wall is a real (if usually small) physical discontinuity for
   anything walking across it on foot, not just a rendering seam. Lower priority than items 1-3 -
   skirts are an accepted, shippable-enough stopgap for a while.
5. [ ] **Antimeridian (UV-seam) handling per chunk** - same fix shape `FixUVSeamsAndFinalize` already
   applies to the whole-sphere mesh, ported to a chunk that straddles U=0/1. Matters for any
   Heightmap/Stamp layer (UV-sampled); noise/procedural layers (direction-sampled) are unaffected.
6. [ ] **Biome/material data threaded into chunk generation.** `AASolarOrbzChunkedPlanetActor` holds
   `BiomeStack` but doesn't forward it yet - a streamed chunk today is terrain-shape only, no biome
   classification, no vertex colors, no per-biome material. Needed before Stage G's ground-level
   visuals mean anything on a REAL (chunked) planet rather than only the bounded preview actor.
7. [ ] **Phase 2 (compute shader) / Phase 3 (Nanite baking)** - deliberately deferred until Phase 1 is
   proven correct and profiling says CPU generation is actually the bottleneck. Don't start either
   before items 1-3 above are done and something is actually on screen to profile.

## Stage C — World-scale coordinate integrity (Not started)

Not mentioned as solved anywhere, and explicitly flagged as NOT addressed by the chunked-terrain work
("Floating-origin/world-rebasing at true planetary distances - inherited from
`ASolarOrbzIcoSphereActor` itself... not solved or worsened by this piece"). This is a hard
prerequisite for "orbit to ground" specifically, not just "big levels in general": a ship actually
transiting from a multi-thousand-kilometer orbit down to a planet's surface crosses the entire range
where float precision matters, in one continuous flight.

- [ ] **Decide the precision strategy and write it down.** UE5.8 ships Large World Coordinates
  (double-precision transforms) by default - confirm this project's `.uproject`/engine build actually
  has `UE_LARGE_WORLD_COORDINATES_DISABLED` unset (it should be, by default, in 5.8), and whether that
  alone is sufficient or whether World Partition's origin-rebasing / a manual floating-origin scheme
  on top is still needed at the distances a full solar system implies (this project already has
  multi-body systems - `ASNSolarSystem`-style content referenced elsewhere - so "one planet's own
  local space" isn't the only distance that matters; transit BETWEEN bodies is too).
- [ ] **Verify chunk generation math holds up at real double-precision radii**, not just the
  visually-convenient preview radii (400 km default) the standalone GUI tool and the bounded preview
  actor use. `FSolarOrbzChunkAddress`/`GetCornerUnitDirections` do their directional math in
  normalized unit-sphere space (precision-safe regardless of planet radius) and only multiply by
  `Radius` at the point of actual vertex placement - confirm this holds for the chunked actor's own
  `RadiusMeters` at Earth-and-larger scale specifically, not assumed from the unit-sphere math being
  radius-independent in theory.
- [ ] **Physics/collision at large radii.** UE's physics engine has its own known precision ceiling
  independent of LWC's rendering-side fix - confirm a player-scale collision check against a
  chunk's mesh stays stable at planetary coordinate magnitudes, not just that it renders correctly.

## Stage D — Gravity, atmosphere & flight model (Not started - see the correction at the top)

Nothing here exists in `all-systems-nominal` yet. This stage is a problem statement, not a spec.

- [ ] **Define the gravity contract a chunked (or whole-sphere) planet actor has to satisfy.**
  `Docs/ChunkedPlanetTerrain.md` already names the right shape even though the implementation doesn't
  exist: gravity direction/magnitude should be derivable from "nearest planet center + radius +
  surface gravity," working identically whether the surface under a given world position is one
  baked mesh (`ASolarOrbzIcoSphereActor`) or a streamed-in chunk
  (`AASolarOrbzChunkedPlanetActor`) - the consuming gravity component shouldn't need to know which.
  `USolarOrbzPlanetProfile` likely already holds (or should hold) the physical values (mass/radius ->
  surface gravity) this needs to read from - confirm against that class directly, don't assume.
- [ ] **A real gravity component**, replacing UE's default world-Z gravity for anything near a
  planet: per-actor "down" becomes "toward the nearest planet's center," with a magnitude that falls
  off (or doesn't, your call) with altitude. Needs to work for a player pawn, loose physics objects,
  and (eventually) vehicles identically.
- [ ] **Atmosphere model** - at minimum, an altitude-to-density function per `USolarOrbzPlanetProfile`
  (`GetAtmosphereDensityAtSeaLevel` already exists and already feeds Climate Simulation's moisture
  scaling - reuse that same number as the actual physics input, don't introduce a second one) feeding
  drag/lift for anything flying, and a hard decision on whether atmospheric entry is a real
  heating/deceleration mechanic or a soft gameplay abstraction. Either is valid; pick one before
  building Stage F's state machine around an assumption.
- [ ] **Flight controller for the orbit/atmospheric-flight regime** - a ship pawn that behaves
  correctly in vacuum (Newtonian, no drag) and in atmosphere (drag/lift-affected) using the same
  underlying physics body, not two different pawns swapped at an arbitrary altitude.

## Stage E — On-foot movement on a curved planet (Not started)

UE5.8's `UCharacterMovementComponent` assumes a fixed world-up vector by default. Walking on a sphere
needs this generalized - a well-understood but genuinely nontrivial problem, not a config flag.

- [ ] **Decide the on-foot movement approach**: a custom `UCharacterMovementComponent` subclass that
  re-derives "up" from the gravity direction every tick (capsule/camera reorientation, floor-normal
  alignment against curved/chunked terrain), vs. a from-scratch movement component if the
  Character/CMC base assumptions fight too much. Research both before committing - this is the single
  highest-uncertainty piece of the whole roadmap because it's the one place UE's own defaults most
  actively assume flat, not spherical, worlds.
- [ ] **Floor detection against chunk boundaries.** A walking character's floor sweep needs to
  succeed cleanly across a skirt-hidden LOD seam (Stage B item 2/4) - test this specifically once
  chunk collision exists, don't assume a working collision mesh is automatically a walkable one at a
  seam.
- [ ] **Camera handling for a reoriented-up character** - third/first-person camera rigs that assume
  world-up need the same gravity-relative treatment as the character capsule itself.

## Stage F — Orbit-to-ground transition (Not started)

The actual "from orbit to walking around" seam the whole roadmap is named after. Depends on C, D, and
E all existing first; this is the integration layer on top, not a parallel independent piece.

- [ ] **A state machine, not an altitude if-check scattered everywhere**: Orbit/Flight -> Atmospheric
  Entry -> Landed/On-Foot (and back), with one authoritative place deciding the current state and
  everything else (camera, controls, HUD, which pawn is possessed) reacting to it.
- [ ] **Pawn-swap or single-pawn-multi-mode?** Decide early - swapping possession between a ship pawn
  and a character pawn is simpler per-mode but needs a well-defined handoff (where does the character
  spawn relative to the now-landed ship, what happens to the ship while on foot); a single pawn that
  changes its own movement component is smoother for the player but harder to get each mode's physics
  right in one class.
- [ ] **Drive Stage B item 3 (spawn budget) from this transition specifically** - this is the exact
  scenario that gap already warns about (a fast altitude change requesting a large resident-chunk-set
  change all at once). Don't let this stage ship before that gap is closed.
- [ ] **Loading/streaming feedback** for the moment chunk generation can't keep up with descent
  speed - a deliberate design decision (slow the ship, show a loading veil, let gaps pop in) not an
  accident to discover during a playtest.

## Stage G — Ground-level content (Partial)

The data pipeline to DESCRIBE what should be on the ground already exists (Stage A). Actually
putting it there, and making it look like more than gray terrain, does not.

- [ ] **Per-biome material blending on the (chunked) terrain mesh.** Today: debug vertex colors on
  the whole-sphere preview actor only; chunks have no biome data at all yet (Stage B item 6). Needs
  an actual landscape-style material (height/slope/biome-ID blended, likely a layered material
  function reading a per-vertex or per-chunk biome-ID channel) - decide this once Stage B item 6
  lands, not before, since the data it blends on doesn't reach a chunk yet.
- [ ] **PCGEx scattering graphs actually built in the UE5.8 editor.** Explicitly deferred earlier in
  this project ("ignore the objects/scattering panel, we can adapt from PCGEx... in the UE5 editor
  instead of standalone") - `USolarOrbzBiomeStack::ExportScatterEntriesToPCGExMeshCollectionJson`
  and the standalone tool's PCG points CSV both already produce the DATA a PCGEx graph would consume;
  no actual graph has been built yet. This is real hands-in-the-editor work this environment cannot
  do itself (no compiler/editor here) - needs a session with a real UE5.8 editor attached.
- [ ] **World Creator's remaining 2 Layer-category entries** (Mask paint, Model/scattering footprint)
  stay deliberately out of scope until there's a real editor to build the interactive tooling in -
  don't attempt either blind.

## Stage H — Persistence / save-load (Not started)

Not designed anywhere yet. A playable planet needs at least:

- [ ] **What's authoritative and regenerable vs. what must be saved.** Procedural terrain/biome output
  is deterministic from (Seed, TerrainStack, BiomeStack, ...) - it doesn't need saving by itself.
  Anything a player CHANGES (if terrain is ever destructible/buildable - not yet decided) does.
- [ ] **Player/ship state**: position (in whatever coordinate scheme Stage C settles on), current
  planet, orbit/flight/landed state (Stage F), inventory/progression if any exists elsewhere in this
  project's design - cross-reference against whatever MMOBackend_Fork's `DBHandler`/`Core/Protocol`
  already assume about player state shape, don't invent a second schema independently.

## Stage I — Multiplayer / networking (Backend skeleton exists, not connected to anything above)

`MMOBackend_Fork` is a real, non-trivial server skeleton (Login service, DBHandler, Core/Protocol,
Core/Services, a TestClient) - this is NOT vaporware the way Stage D's gravity/atmosphere layer is,
but it's also not yet wired to anything terrain/gameplay-related.

- [ ] **Decide scope-of-truth for a chunked planet in a networked session early** - this changes
  Stage B's own design meaningfully if decided late. Likely shape: terrain is deterministic from
  shared (Seed, TerrainStack, BiomeStack) so chunk GEOMETRY doesn't need server replication, only
  authoritative player/ship state (Stage H) does - confirm this assumption explicitly rather than
  discovering a terrain desync bug after the fact.
- [ ] **Decide if Stage F's state machine (orbit/flight/landed) needs server authority** at all, or
  is purely client-predicted/cosmetic until a player-state-affecting event (landing, combat, trading)
  needs the server to care.
- [ ] **Don't let single-player-only assumptions calcify into Stage D/E/F's APIs** if multiplayer is a
  real near-term target - worth deciding THIS early, since retrofitting network authority onto a
  gravity/movement system designed single-player-only is a much bigger rewrite than designing it in
  from the start.

## Stage J — Performance & optimization (Research exists, nothing implemented)

`Docs/UE5EngineOptimizationResearch.md` is a holding pen of engine-level candidates (ray tracing BLAS
residency, incremental GC, and others) explicitly scoped to what THIS project stresses - planet-scale
chunked streaming, heavy `ParallelFor` use, LWC math, an eventual MMO client/server split. Nothing in
it has been implemented; it's a reference to pull from once Stage B is actually running and there's
something real to profile, not a checklist to work through blind.

- [ ] Revisit once Stage B items 1-3 are done and playable - profile against real hardware, don't
  flip engine CVars speculatively first (that doc's own standing caution).

---

## Suggested order of attack

Stages don't have to fully complete before the next starts, but respect these real dependencies:

1. **B (items 1-3) first** - nothing below can be tested standing on real ground without working,
   collidable, budget-aware chunk streaming. This is the actual critical path.
2. **C in parallel with B** - coordinate precision has to be right before any of B's playtesting
   results can be trusted at real planetary radii rather than the small preview radii used so far.
3. **D and E can start as soon as B has collidable chunks** (even before B's items 4-7) - gravity and
   on-foot movement both need *something* to stand on, not the fully-polished version.
4. **F depends on C, D, and E all existing** - it's the seam between them, not a parallel track.
5. **G (materials/scattering) and H (persistence) can proceed anytime after B item 6** (biome data
   reaching a chunk) **and F** (state to persist) respectively - genuinely parallelizable with
   everything above once their own prerequisite lands.
6. **I (multiplayer) needs an early SCOPE decision (see Stage I's own first item) but not early
   implementation** - deciding "what does the server need to know" now is cheap; building the
   networking layer itself can wait until there's a working single-player loop to network.
7. **J last, and only against something real to profile.**

## Open questions this roadmap can't answer for you

- Is multiplayer a near-term target or a someday-maybe? This changes how much Stage D/E/F should be
  designed with network authority in mind from day one (see Stage I's own warning).
- Is there a vehicle/mech layer (the "ASNMechLab" referenced elsewhere in this project) that's the
  PRIMARY way a player traverses a planet's surface, with "on foot" secondary or absent? That
  changes Stage E's priority substantially - confirm before investing heavily in on-foot movement
  specifically.
- Real-world planetary scale (thousands of km radius, Stage C's full precision problem) vs. a
  deliberately smaller/stylized scale (sidesteps much of Stage C, at the cost of "this isn't really
  Mars-sized") - a legitimate, common choice in this genre, worth deciding consciously rather than
  defaulting into real scale by not thinking about it.
