# Chunked / Streaming Planet Terrain — Design

**Status: design + first foundational piece (cube-sphere chunk math) only. Nothing here streams,
LODs, or renders yet.** Written without a UE5.8 compiler available (see README's standing caveat on
every other SolarOrbz doc) - read code comments for the same "verify before relying on this" flags
`SolarOrbzIcoSphere.cpp` already uses for winding/orientation-sensitive math.

## Why this exists

`ASolarOrbzIcoSphereActor` (see `ROADMAP.md`'s "Terrain not moving vertices enough at Earth scale"
entry) is architecturally a **single mesh** - one `UProceduralMeshComponent` built from one
`FSolarOrbzIcoSphereMeshData`. No subdivision level makes that mesh show ground-level detail at a
real planetary radius (~6.37M meters for Earth): `EstimateVertexCount` is already in the billions
before you'd get there. That's fine - that actor is explicitly scoped as "a good fit for a bounded
preview/bake radius, or zoomed-in testing," not a final answer, and this document doesn't change
anything about it.

This is the actual answer: a planet's surface is split into many small **chunks**, only the chunks
near the camera/player exist as real geometry at any given time, and farther chunks are coarser (or
don't exist at all yet). This is a second, parallel system - `ASolarOrbzIcoSphereActor` keeps doing
exactly what it does today for anyone who wants a bounded, fully-resident test body.

## Topology: cube-sphere, not icosphere-quadtree

The icosphere generator (`FSolarOrbzIcoSphereGenerator`) stays exactly as it is - this is a second,
independent mesh generator (`FSolarOrbzCubeSphereChunkGenerator`, see below), not a replacement.

A **cube-sphere** (6 quadtree-subdivided cube faces, each face's quadtree node projected onto the
sphere) is the chunking scheme, not a quadtree built directly over the icosphere's triangles. Reasons:

- **Uniform quad chunks.** Each cube face chunk is a plain NxN grid - trivial to generate, trivial
  neighbor-finding (exactly 4 neighbors per chunk, same depth). An icosphere-based chunking would need
  *triangular* chunks, and every icosphere vertex has 5 or 6 neighbors (the 12 original icosahedron
  vertices are permanently 5-valent) - workable, but every piece of neighbor/seam logic needs a special
  case at those 12 points that a cube-sphere's 8 corners (3 faces meeting) and 12 edges (2 faces
  meeting) don't need nearly as much of.
- **Industry precedent.** This is the standard approach for planet-scale terrain in the space-sim/
  planet-renderer space (Outerra and most "quadtree planet" implementations use it) - not a novel
  choice that still needs its own research pass.
- **The existing terrain recipe doesn't care.** `USolarOrbzTerrainLayerStack::EvaluateHeight(UnitDirection,
  UV, ClimateGrid)` only ever takes a direction vector + a UV - it has no idea whether that direction
  came from an icosphere vertex or a cube-sphere grid point. Every existing `USolarOrbzTerrainLayer`/
  `USolarOrbzBiomeMask`/planet `Profile`/`USolarOrbzClimateSimulationAsset` asset keeps working
  completely unchanged; a planet authored today for the preview actor can be pointed at the chunked
  system later with the same `TerrainStack`/`BiomeStack`/`ClimateSimulation`/`Profile` references.

## Where Nanite actually fits

Nanite is **not** a live "generate arbitrary dynamic geometry and auto-LOD it every frame" system -
it's a rendering pipeline for virtualized, built geometry, with its own cluster-building step
(`UStaticMesh::BuildFromMeshDescriptions` with `FMeshNaniteSettings::bEnabled = true` - the exact hook
already sitting unused in `ASolarOrbzIcoSphereActor::BakeToStaticMeshAsset`, currently forced off with
a `// flip on later once you're baking at final terrain density` comment). So concretely, this is a
**two-level LOD system**, not Nanite doing everything:

- **Coarse level (ours to build): which chunks exist at all right now.** A quadtree-per-cube-face
  streaming manager, driven by camera/player distance, decides which `FSolarOrbzChunkAddress` nodes
  are currently resident, generates/bakes them, and evicts ones that fall out of range. This is
  ordinary gameplay-code streaming logic (not a rendering feature) - closer to how `World Partition`/
  landscape streaming proxies work than anything Nanite-specific.
- **Fine level (Nanite's job, once a chunk is built): geometric detail within a resident chunk.**
  Each generated chunk gets baked the same way `BakeToStaticMeshAsset` already bakes the preview
  actor's whole mesh (same `FMeshDescription`/`FStaticMeshAttributes` path), just with Nanite turned on
  and at chunk scale instead of whole-planet scale. Nanite then handles that chunk's own internal
  LOD/clustering automatically at render time.

## Where a compute shader fits (phased, not day one)

Porting the *entire* terrain recipe to HLSL in one pass is its own large, separate project - the mask
system supports arbitrary recursive Composite Masks (`USolarOrbzCompositeBiomeMask`, depth-capped at
16, see `SolarOrbzBiomeSystem.cpp`), the layer stack is a variable-length list of virtually-dispatched
`USolarOrbzTerrainLayer` UObjects, and Erosion bakes a whole-surface iterative grid pass before any
per-point query. None of that has an obvious 1:1 HLSL translation, and a bad port can't be caught by a
compiler the way the C++ side's mistakes at least sometimes can.

Phased plan, in order - **do not start Phase 2 before Phase 1 is proven to work**:

1. **Phase 1 (this and the next few passes): CPU-chunked streaming, prove the architecture.** Build
   the cube-sphere chunk generator (below), the quadtree streaming manager, neighbor/LOD-seam
   handling, and ASN_MK1 integration (gravity/atmosphere need a chunked-aware body, see "ASN_MK1
   integration" below) all using the *existing* CPU `EvaluateHeight` path, parallelized with
   `ParallelFor` the same way `ASolarOrbzIcoSphereActor::RegenerateMesh` now is (see `ROADMAP.md`'s
   "Regenerate performance" entry) - one `ParallelFor` per chunk's vertex grid, and chunk generation
   itself farmed out across available chunks too. This is deliberately *not* GPU work yet - it proves
   out the hard, architecture-defining problems (chunk topology, streaming/eviction, seams, how a
   chunked planet presents itself to gravity/atmosphere/the rest of the game) with a debugger
   attached and `UE_LOG` available, before adding GPU debugging's much higher cost of iteration on
   top.
2. **Phase 2 (only once Phase 1 streams/looks right, if profiling says CPU generation is actually the
   bottleneck): move the common-case height evaluation to a compute shader.** Likely shape: a compute
   shader fills a per-chunk heightfield buffer directly from the noise/simple-mask common case (Noise/
   Planetary Noise/Continent/Terrace layers, masks that don't need recursion or climate data) for the
   large majority of chunks that only use those, while the long-tail (deep composite-mask recursion,
   Erosion's whole-surface bake) either falls back to the existing CPU path per chunk, or gets
   precomputed once per planet into a low-res equirectangular "detail" texture the shader samples
   instead of evaluating live (same bake-once-sample-many shape Erosion/Climate Simulation already use
   today, just moved to a texture the GPU can read).
3. **Phase 3 (optional, only if Phase 2 isn't enough): Nanite-enabled chunk bakes**, per "Where Nanite
   actually fits" above, once chunk generation itself is fast and correct.

## ASN_MK1 integration (not yet started)

`all-systems-nominal`'s `AASNPlanetActor`/`UASNPlanetGravityComponent` currently assume a planet is
exactly one actor with one mesh and a `BodyRadiusMeters`/`SurfaceGravityMPS2` pair (see that repo's
`Wiki/ASNSolarSystem.md`). A chunked planet needs to present the same contract - gravity should keep
working from `GetActorLocation()` + radius regardless of how many chunk sub-actors/components exist
underneath, and atmosphere sampling (`IASNAtmosphereSource`) likewise shouldn't care whether the
surface under a given world position is one baked mesh or a streamed-in chunk. The concrete shape of
"one owning actor, many chunk children" isn't designed yet - flagged here so it isn't forgotten, not
blocking Phase 1's cube-sphere/chunk-generation work starting in SolarOrbz itself first.

## What's actually built so far (this pass)

- **`FSolarOrbzChunkAddress`** (`SolarOrbzCubeSphereChunk.h`) - a quadtree node's identity: which of
  the 6 cube faces, subdivision depth, and (X, Y) coordinates within that face at that depth.
  `GetParent()`/`GetChildren()` for walking the tree; no streaming/residency logic yet, this is pure
  addressing.
- **`FSolarOrbzCubeSphereChunkGenerator`** (`SolarOrbzCubeSphereChunk.h/.cpp`) - generates one chunk's
  mesh data (reuses `FSolarOrbzIcoSphereMeshData` - a grid of displaced vertices is a grid of displaced
  vertices, regardless of which generator produced it) for a given `FSolarOrbzChunkAddress` + grid
  resolution + radius, optionally displaced by an existing `USolarOrbzTerrainLayerStack` exactly the
  way `RunTerrainPassA` displaces the icosphere today. `TerrainStack == nullptr` generates an
  undisplaced sphere patch - useful for testing chunk topology/seams in isolation before wiring in real
  terrain.

**Known, deliberately out of scope for this pass** (do not assume these are solved):

- **No neighbor/LOD-seam stitching yet.** Two adjacent chunks at different quadtree depths will show a
  visible crack/T-junction where their edge vertex densities don't match - a well-known, well-solved
  problem (skirts, edge morphing, or simply never letting neighboring resident chunks differ by more
  than one LOD level), just not implemented here. Don't stream mixed-LOD neighbors in yet.
- **No normalize-only distortion correction.** `FaceLocalToUnitSphereDirection` projects a cube point
  onto the sphere via plain normalization, which is NOT area-preserving (chunks near a cube corner
  cover less surface area than chunks near a face center, at the same quadtree depth). The standard
  fix is a "COBE quad-sphere" warp on the face-local (S,T) before projecting - worth adding once chunk
  density differences are actually visible in practice, not guessed at now.
- **No antimeridian handling.** A chunk whose footprint straddles the equirectangular UV seam (U
  wrapping 0->1, exactly the case `FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize` exists to fix
  for the whole-sphere mesh) will get a badly-interpolated V... er, U coordinate across that chunk,
  which matters for any Heightmap/Stamp layer sampling via UV. Rare in practice (most chunks are far
  from the seam), but real; same fix shape as the icosphere's (duplicate/offset U per-triangle) would
  apply per-chunk, not implemented yet.
- **No streaming manager, no LOD-selection-by-camera-distance, no baking/Nanite path, no ASN_MK1
  integration.** All of "Phase 1" above beyond the chunk generator itself.
- **Face basis vectors/winding are UNVERIFIED** - written without a compiler. `GetFaceBasis`'s six
  per-face (Right, Up, Forward) triples need to be checked against an actual rendered chunk before
  trusting seams line up between faces, the same way `SolarOrbzIcoSphere.cpp`'s own header flags its
  `FMatrix` constructor assumption. **Test this before building anything else on top of it.**
