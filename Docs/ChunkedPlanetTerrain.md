# Chunked / Streaming Planet Terrain — Design

**Status: design + first foundational piece (icosphere-based chunk math) only. Nothing here streams,
LODs, or renders yet.** Written without a UE5.8 compiler available (see README's standing caveat on
every other SolarOrbz doc) - read code comments for the same "verify before relying on this" flags
`SolarOrbzIcoSphere.cpp` already uses for winding/orientation-sensitive math.

**Revision note:** the first pass of this document and its foundational code used a cube-sphere
chunking scheme (6 quadtree-subdivided cube faces). Superseded, by explicit request, in favor of
building the chunk system directly on `FSolarOrbzIcoSphereGenerator`'s own base-icosahedron geometry
instead - see "Topology" below for why this is actually lower-risk, not just a stylistic preference.
The cube-sphere files (`SolarOrbzCubeSphereChunk.h/.cpp`) were deleted rather than kept alongside this
version - there is one chunked system, not two competing ones.

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

## Topology: a quadtree over the icosphere generator's own 20 base faces

The whole-sphere icosphere generator (`FSolarOrbzIcoSphereGenerator`) stays exactly as it is and keeps
serving `ASolarOrbzIcoSphereActor` unchanged - this is a second, parallel mesh generator
(`FSolarOrbzIcoSphereChunkGenerator`, see below), not a replacement. But it deliberately **reuses that
generator's own base-icosahedron table and recursive subdivision scheme** rather than inventing a
second, independent geometric basis:

- **One chunk address = one path down the exact same 4-way split `SubdivideOnce` already performs.**
  `BuildBaseIcosahedron`'s 20 triangular faces are the quadtree's 20 roots; each `+1` depth quarters
  the current triangle into the same 4 children (3 corner triangles + 1 center triangle) that function
  already produces for the whole mesh - `FSolarOrbzChunkAddress::GetCornerUnitDirections` just walks
  that same split down to a chosen depth instead of applying it everywhere and emitting one giant mesh.
  Concretely: `FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron()` is a new public accessor exposing the
  exact 12 vertices/20 faces `BuildBaseIcosahedron` already builds from, so the chunk system reads the
  same numbers rather than a second, possibly-drifting copy.
- **Meaningfully lower-risk than the cube-sphere version this replaced.** That version needed 6
  hand-derived per-face (Right, Up, Forward) basis vector triples - new geometry, unverified without a
  renderer. This version's corner-resolution math is a strict subset of logic the whole-sphere
  generator already relies on (and whatever confidence that code has earned from being the generator
  `ASolarOrbzIcoSphereActor` actually uses, this inherits) - what's actually new here is only the
  *triangulation* of each chunk's own internal vertex grid (a barycentric subdivision of one triangle,
  not full-mesh subdivision), which is still unverified without a renderer like everything else in this
  pass.
- **The existing terrain recipe still doesn't care.** `USolarOrbzTerrainLayerStack::EvaluateHeight(UnitDirection,
  UV, ClimateGrid)` only ever takes a direction vector + a UV - it has no idea whether that direction
  came from the whole-sphere mesh or one chunk's local grid. Every existing `USolarOrbzTerrainLayer`/
  `USolarOrbzBiomeMask`/planet `Profile`/`USolarOrbzClimateSimulationAsset` asset keeps working
  completely unchanged; a planet authored today for the preview actor can be pointed at the chunked
  system later with the same `TerrainStack`/`BiomeStack`/`ClimateSimulation`/`Profile` references.
- **Real tradeoff, now partly addressed: the 12 original icosahedron vertices are permanently
  5-valent** (5 neighbors instead of 6) - every piece of neighbor-finding/seam logic eventually needs a
  special case at those 12 points that a cube-sphere's uniform 4-neighbor quads wouldn't have needed.
  Accepted deliberately in exchange for the lower-risk geometry reuse above. **The lookup primitive for
  this special case now exists** - `FSolarOrbzChunkAddress::IsAnchoredAtOriginalVertex`/
  `GetPentagonVertexNeighbors` (see "What's actually built so far" below) resolve, for a chunk whose own
  corner sits exactly at one of the 12 points, the other same-depth chunks (one per other base face)
  sharing it - verified independently against the real 20-face table (all 12 vertices confirmed
  touched by exactly 5 faces, both the simple all-same-slot case and a mixed-slot case). Still not
  wired into any actual seam-stitching (no general, valence-6 neighbor-finding exists yet either - see
  "known out of scope" below) - this is the special-case primitive the general system will need to
  call, built first so it isn't discovered/reverse-engineered after the general system already assumes
  6 everywhere.

## Where Nanite actually fits

Nanite is **not** a live "generate arbitrary dynamic geometry and auto-LOD it every frame" system -
it's a rendering pipeline for virtualized, built geometry, with its own cluster-building step
(`UStaticMesh::BuildFromMeshDescriptions` with `FMeshNaniteSettings::bEnabled = true` - the exact hook
already sitting unused in `ASolarOrbzIcoSphereActor::BakeToStaticMeshAsset`, currently forced off with
a `// flip on later once you're baking at final terrain density` comment). So concretely, this is a
**two-level LOD system**, not Nanite doing everything:

- **Coarse level (ours to build): which chunks exist at all right now.** A quadtree-per-base-face
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
   the icosphere chunk generator (below), the quadtree streaming manager, neighbor/LOD-seam
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
blocking Phase 1's icosphere-chunk-generation work starting in SolarOrbz itself first.

## What's actually built so far (this pass)

- **`FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron()`** (new public accessor, `SolarOrbzIcoSphere.h/
  .cpp`) - exposes the generator's own 12 base vertices/20 base faces, which `BuildBaseIcosahedron` now
  sources from too (one table, not a duplicate). Nothing about the existing whole-sphere generator's
  behavior changed - this is a pure refactor (expose existing private data), not a new algorithm.
- **`FSolarOrbzChunkAddress`** (`SolarOrbzIcoSphereChunk.h`) - a quadtree node's identity: which of the
  20 base icosahedron faces, subdivision depth, and a packed `PathBits` recording which of the 4
  children was chosen at every level down to that depth. `GetParent()`/`GetChildren()` for walking the
  tree; `GetCornerUnitDirections()` resolves the chunk's actual 3 corner directions by replaying
  `SubdivideOnce`'s own corner-child/center-child split down `PathBits`. No streaming/residency logic
  yet, this is pure addressing.
- **`FSolarOrbzIcoSphereChunkGenerator`** (`SolarOrbzIcoSphereChunk.h/.cpp`) - generates one chunk's
  mesh data (reuses `FSolarOrbzIcoSphereMeshData`) for a given `FSolarOrbzChunkAddress` + per-edge
  subdivision count + radius, optionally displaced by an existing `USolarOrbzTerrainLayerStack` exactly
  the way `RunTerrainPassA` displaces the whole-sphere mesh today. Fills the chunk's triangle via
  barycentric interpolation of its 3 corners (re-normalized onto the unit sphere per vertex) rather than
  recursively re-subdividing down to the target resolution - simpler and resolution-independent (doesn't
  require the within-chunk density to be a power of 2). `TerrainStack == nullptr` generates an
  undisplaced sphere patch - useful for testing chunk topology/seams in isolation before wiring in real
  terrain.
- **`FSolarOrbzChunkAddress::IsAnchoredAtOriginalVertex` / `GetPentagonVertexNeighbors`** (the "5-valent
  fix", `SolarOrbzIcoSphereChunk.h/.cpp`) - the pentagon-vertex special case from "Topology" above, as a
  standalone lookup. `IsAnchoredAtOriginalVertex` recognizes a chunk whose own corner sits exactly at
  one of the 12 original icosahedron vertices (level 0 of its `PathBits` picked which original corner
  to anchor to - 0/1/2, never the center child 3 - and every level after that stayed on child 0 to keep
  that exact point pinned rather than sliding onto a midpoint). `GetPentagonVertexNeighbors` then walks
  `GetBaseIcosahedron`'s 20-face table for every OTHER face touching that same original vertex and
  builds the matching same-depth `FSolarOrbzChunkAddress` for each - always exactly 4 (5 faces touch
  each original vertex, minus the querying chunk's own). Checked independently against the real face
  table outside the engine (both the simple case where every touching face shares the same corner-slot,
  and a mixed-slot case where they don't) - see the commit history for the verification script; all 12
  vertices confirmed exactly 5-valent, both test shapes matched by hand. **Lookup only** - nothing
  consumes this yet to actually stitch geometry.

**Known, deliberately out of scope for this pass** (do not assume these are solved):

- **No neighbor/LOD-seam stitching yet**, still - the pentagon-vertex lookup above is the special-case
  primitive that kind of stitching will need, not the stitching itself. Two adjacent chunks at different
  quadtree depths will show a visible crack/T-junction where their edge vertex densities don't match - a
  well-known, well-solved problem (skirts, edge morphing, or simply never letting neighboring resident
  chunks differ by more than one LOD level), just not implemented here. Don't stream mixed-LOD neighbors
  in yet.
- **No general (valence-6, non-pentagon) neighbor-finding either.** Finding "the chunk across this edge"
  for an ordinary interior or base-face-boundary edge needs its own table (which of the 30 shared edges
  between the 20 base faces connects to which, and in what orientation/flip) - not built yet, and not
  the same problem `GetPentagonVertexNeighbors` solves (that one is specifically about the 12 vertices,
  not edges in general).
- **Distortion is a smaller concern here than the cube-sphere version had, but not zero.** Barycentric
  interpolation across a base triangle's 3 corners, re-normalized per vertex, is the same kind of
  geodesic subdivision `SubdivideOnce` already performs for the whole mesh - a long-established,
  comparatively mild distortion profile (this is a large part of why icospheres are generally preferred
  over cube-spheres for even triangle sizing). Not re-derived or specifically measured for the chunked
  case here, just inherited from a scheme already in production use elsewhere in this file.
- **No antimeridian handling.** A chunk whose footprint straddles the equirectangular UV seam (U
  wrapping 0->1, exactly the case `FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize` exists to fix
  for the whole-sphere mesh) will get a badly-interpolated U coordinate across that chunk, which matters
  for any Heightmap/Stamp layer sampling via UV. Rare in practice (most chunks are far from the seam),
  but real; same fix shape as the whole-sphere mesh's (duplicate/offset U per-triangle) would apply
  per-chunk, not implemented yet.
- **No streaming manager, no LOD-selection-by-camera-distance, no baking/Nanite path, no ASN_MK1
  integration.** All of "Phase 1" above beyond the chunk generator itself.
- **Triangulation winding is UNVERIFIED** - written without a compiler or renderer.
  `FSolarOrbzIcoSphereChunkGenerator::GenerateChunk`'s within-chunk grid triangulation applies the same
  "swap the last two corners" empirical fix `FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize`
  documents, by analogy rather than by independent verification. **Test a generated chunk's front/back
  facing before building anything else on top of it.**
