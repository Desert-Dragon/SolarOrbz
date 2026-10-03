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
  touched by exactly 5 faces, both the simple all-same-slot case and a mixed-slot case). **The general
  (valence-6, non-pentagon) edge-neighbor finder now exists too** -
  `FSolarOrbzChunkAddress::GetEdgeNeighbor`, exhaustively verified (depths 0-6, every base face, every
  edge - 327,660+ cases, zero failures) against this struct's own already-shipped
  `GetCornerUnitDirections` before being ported to C++; see "What's actually built so far" below for
  how a first attempt at this (another flat-grid shortcut) was caught failing and discarded the same
  way the cube-sphere topology itself was. Pentagon-vertex chunks are still correctly handled by
  `GetEdgeNeighbor` for their EDGES (an edge touching a 5-valent vertex at one endpoint is still an
  ordinary 2-chunk boundary) - `GetPentagonVertexNeighbors` answers a different question ("every chunk
  touching this exact point," useful for the vertex-fan case seam-stitching will eventually need, not
  just edge-by-edge continuity). Neither is wired into any actual seam-stitching yet - see "known out
  of scope" below.

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

## Phase 1, continued: the streaming/residency manager (plan, nothing built yet)

Everything above this point answers "what is chunk X, and what's next to it." Nothing yet answers
"which chunks should exist right now, and what happens as the viewer moves" - that's this piece. Plan
only below; see the checklist for build order. Each numbered subsystem is written up so it can be
built and verified independently, same discipline as addressing/generation/neighbor-finding above -
anything non-trivial (point-location, the LOD-restriction fixpoint) gets a Python ground-truth check
before any C++ is written, not after.

1. **Point-location: "which chunk contains this world direction?"** Doesn't exist yet, and the
   streaming manager can't decide anything without it - it needs to know where the viewer actually is
   in chunk-space before it can pick a resident set. Shape: a point-in-spherical-triangle test to find
   which of the 20 base faces contains a given unit direction, then the same test recursively against
   each of the 4 children (via `GetChildren`'s corners) to descend to a target depth, or until a
   caller-supplied "stop here" predicate (the LOD policy below) says this node is fine as a leaf.
2. **LOD policy: "what depth should a given chunk be, for a viewer at this position?"** A pure
   function, no engine/actor dependencies - given a chunk's approximate world-space size (radius *
   angular span at its depth) and distance from the viewer to the chunk (nearest point or centroid is
   fine for Phase 1), decide split/stay. Doesn't need to be perfect - directionally correct (closer or
   bigger-on-screen => deeper) is enough to prove the architecture; tuning the exact threshold is a
   later pass once something is actually on screen to look at.
3. **Quadtree residency walk (unrestricted).** Starting from the 20 roots, recursively apply the LOD
   policy: split while it says to, stop (this is a candidate resident leaf) when it doesn't, hard-stop
   at `FSolarOrbzChunkAddress::MaxDepth` regardless. Produces a naive desired leaf-set with no
   constraint on how much neighboring leaves' depths can differ yet.
4. **Restricted-quadtree fixpoint (the actual reason `GetEdgeNeighbor` got built first).** Real-time
   terrain LOD systems universally cap how much a resident chunk's depth can differ from its
   neighbors' (usually 1 level) - skip this and the seam-hiding step below has to handle arbitrarily
   large cracks instead of always-one-level ones, which is a much harder problem. Pass: for every
   candidate leaf, check its 3 edge-neighbors (`GetEdgeNeighbor`) and its corners' pentagon-vertex
   neighbors where applicable (`GetPentagonVertexNeighbors` - decide during implementation whether
   Phase 1 actually needs the vertex-fan case enforced, or whether skirts alone make it moot); if a
   neighbor is shallower by more than 1 level, force-split it (which may itself now violate the
   constraint against ITS OWN neighbors - iterate to a fixpoint, same as every other implementation of
   this well-known constraint).
5. **Resident-set diffing + chunk lifecycle.** Keep a map of currently-resident
   `FSolarOrbzChunkAddress -> UProceduralMeshComponent`. Each update: recompute the desired leaf-set
   (1-4 above), diff against what's currently resident, spawn newly-needed chunks
   (`FSolarOrbzIcoSphereChunkGenerator::GenerateChunk`, `ParallelFor`'d across the newly-needed set the
   same way `ASolarOrbzIcoSphereActor::RegenerateMesh` now parallelizes its own per-vertex work),
   despawn/destroy no-longer-needed ones, leave unchanged chunks alone (this should be the common case
   once the viewer stops moving - don't regenerate chunks that don't need it).
6. **Skirts, not true edge-stitching, for Phase 1's seam hiding.** With neighbor depth capped to a
   1-level difference, the remaining crack at every differing-LOD edge is small and bounded - the
   standard cheap fix (used broadly in terrain engines) is a "skirt": extend each chunk's boundary
   vertices inward/downward by a fixed amount so any gap is hidden by a near-vertical wall instead of
   showing through. Real vertex-welding (actually matching densities across the edge) is more correct
   and explicitly deferred - skirts first, because they're simple, proven, and don't block anything
   else in this list.
7. **`AASolarOrbzChunkedPlanetActor` (new, parallel to `ASolarOrbzIcoSphereActor`, not a replacement).**
   Owns the same kind of references the preview actor does (Radius, `TerrainStack`/`BiomeStack`/
   `ClimateSimulation`/`Profile`), plus a registered viewer (a camera manager reference, or just an
   explicit world position for Phase 1 testing) and an interval-driven (not necessarily every-tick -
   residency doesn't need to recompute every frame) update calling into 1-6 above.

**Explicitly out of scope for this piece, same as everything else in this doc** - don't assume any of
these are solved once the above lands:
- Real vertex-stitching across differing-LOD edges (skirts only, see item 6).
- Any per-chunk budget/prioritization for spawning many chunks in one update (a sudden large viewer
  jump - e.g. teleport, not just normal flight - could request many chunks at once; a frame-budget/
  queue system is real future work, not built here).
- Floating-origin/world-rebasing at true planetary distances - inherited from `ASolarOrbzIcoSphereActor`
  itself (see its own header), not solved or worsened by this piece.
- Baking/Nanite (Phase 3) and compute-shader generation (Phase 2) - unchanged from the phasing above.
- ASN_MK1 integration - still next, see below, now genuinely blocked on this piece existing first
  rather than on SolarOrbz work.

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
- **`FSolarOrbzChunkAddress::GetEdgeNeighbor`** (the general, non-pentagon edge-neighbor finder,
  `SolarOrbzIcoSphereChunk.h/.cpp`) - "the same-depth chunk across this edge," for any edge at any
  depth, crossing a base-face boundary via `GetBaseIcosahedron`'s own data when needed. Standard
  quadtree neighbor-finding (Samet-style ascend-to-a-resolvable-ancestor, then descend back down),
  generalized to a triangular 4-child (3 corner + 1 center) split instead of a square one.
  - **A first attempt at this failed, and that failure is worth recording.** The first design tried a
    flat `(Row, Column, Up/Down-orientation)` grid addressing scheme instead of a path - modeled on
    how the (now-abandoned) cube-sphere version addressed chunks - specifically to get O(1) neighbor
    arithmetic instead of a recursive walk. Brute-force verification against this struct's own
    `GetCornerUnitDirections` caught that it was wrong: this project's actual subdivision
    (`SubdivideOnce`, and `GetCornerUnitDirections`) re-normalizes onto the sphere at **every** split
    level, while a flat grid only interpolates once across the root corners - the two agree exactly
    through depth 1, then silently diverge. Caught before being committed; nothing broken was ever
    shipped.
  - **The actual, shipped algorithm works directly on `PathBits`**, no parallel coordinate system.
    Every (child index, edge) combination is either **internal** (shared with one specific sibling
    within the same 4-way split - e.g. a corner child's "BC" edge is always shared with the center
    child) or **boundary** (half of the parent's corresponding edge, nearer one specific parent
    corner). Ascending while the edge keeps resolving as boundary, then descending back down once it
    resolves to a sibling (or the base-face root, requiring a cross-face jump via
    `GetBaseIcosahedron`'s edge-adjacency), finds the same-depth neighbor. One non-obvious thing the
    first few iterations got wrong and brute-force verification caught each time: **both an
    internal-sibling match and a cross-face jump reverse the edge's traversal direction** (confirmed
    directly - every one of the 30 shared base-face edges is traversed in opposite vertex order by its
    two faces, and every corner-child/center-child internal pairing likewise faces opposite ways) -
    miss flipping the accumulated "which side" bookkeeping at either kind of transition and the
    descent lands one sibling off from the real answer.
  - **Verification: exhaustive, not spot-checked, before any of this was ported to C++.** Every
    `(base face, depth, path, edge)` combination for depths 0-6 across all 20 base faces (327,660+
    cases) checked that the computed neighbor shares exactly 2 of its 3 corners with the query chunk,
    using this struct's own already-shipped `GetCornerUnitDirections` as ground truth - zero failures.
    Random spot checks extended that to depth 20 (300 cases per depth, also zero failures). The C++
    lookup tables were then cross-checked entry-by-entry against the verified Python source before
    being treated as done. Same standing caveat as everything else in this plugin: the *algorithm* was
    checked rigorously outside the engine; this specific C++ transcription of it has not been compiled
    or run.
- **`FSolarOrbzChunkPointLocator::FindChunkContainingDirection`** (Phase 1, continued, item 1 -
  `SolarOrbzChunkPointLocation.h/.cpp`, new files) - "which chunk at depth D contains this world
  direction?" Point-in-spherical-triangle test (`Dot(Cross(A,B),P) >= 0` on all 3 edges - a single
  fixed sign works for every triangle this project produces, no opposite-corner comparison needed,
  since `GetBaseIcosahedron`/`GetCornerUnitDirections` never vary their CCW-from-outside winding) to
  classify into one of the 20 base faces, then the same test recursively against `GetChildren`'s 4
  children to descend to the target depth. Boundary ties (a point exactly on a shared edge) resolve by
  first-match-wins in ascending index order - arbitrary but deterministic, which is all a zero-width
  boundary needs. Verified independently twice: the implementing agent's own Python ground truth
  (259,000+ cases: winding-sign check, exhaustive depth-0-5 round-trip, random depth 6-20 sampling,
  explicit boundary tie-break checks - zero failures), then re-derived and re-checked from scratch
  again afterward (not reusing the agent's scripts) with a harsher random-interior-point round-trip
  across depths 0-18 (3,200 cases) plus all 30 shared base-face edge midpoints - zero failures both
  times. No LOD-aware early-stop yet (always walks to exactly `TargetDepth`) - see its header for why
  that's deliberately deferred to the LOD policy below.
- **`FSolarOrbzChunkLODPolicy`** (Phase 1, continued, item 2 - `SolarOrbzChunkLODPolicy.h/.cpp`, new
  files) - "should this one chunk split (or merge back), for a viewer at this position?" A pure,
  engine-free function: `(chunk's longest edge) / (nearest of: distance to each of its 3 corners,
  distance to its centroid re-projected onto the sphere)`, compared against a tunable threshold
  (`FSolarOrbzChunkLODSettings`, default `SplitScreenSizeRatio = 1.0`). Longest edge (not
  shortest/average) because a thin sliver chunk near a pentagon vertex still needs splitting if even
  one edge is huge on screen; nearest-of-corners-and-centroid (not corners alone) because a viewer
  near the interior of a still-large, still-shallow chunk can be far from all 3 corners yet genuinely
  close to the surface. Hysteresis via two separate thresholds/functions (`ShouldSplit` at the higher
  `SplitScreenSizeRatio`, `ShouldMerge` at the lower `MergeScreenSizeRatio = 0.75`) rather than one
  function compared both ways - the gap between them is what actually stops a chunk sitting at the
  boundary from flickering every update; a remaining gap (sustained oscillation across that gap over
  many updates needs per-chunk resident state + an update cadence) is explicitly left for the residency
  manager (item 5), not solved here. This is a tuned heuristic, not an exact topological claim, so the
  verification bar is different from the neighbor-finding work above: checked by hand-computed ratios
  across the realistic range (Earth-like radius, depths 0-24, viewer altitudes from a 400 km orbit down
  to 1.8 m eye height) confirming the default threshold lands in a sane middle ground (settles at Depth
  23 standing on the ground, Depth 5 in low orbit - neither "wants MaxDepth with no margin" nor "stays
  shallow"), re-derived independently afterward against a from-scratch Python port of the exact same
  formula - every quoted ratio matched to the reported precision, plus monotonicity (ratio strictly
  increases as viewer approaches a fixed chunk; strictly decreases with depth at a fixed viewer
  position) held with zero failures across the full practical range. Not wired into any tree walk yet -
  that's items 3/4.
- **`FSolarOrbzChunkResidencyWalker::GatherDesiredLeaves`** (Phase 1, continued, item 3 -
  `SolarOrbzChunkResidencyWalk.h/.cpp`, new files) - "starting from the 20 base faces, recursively
  applying the LOD policy, what is the resulting desired leaf-set for a viewer at a given position?"
  Pure recursion: at each node, resolve its 3 corners (`GetCornerUnitDirections`, scaled by `Radius`
  - the one place that scaling happens, so callers don't do it themselves), ask
  `FSolarOrbzChunkLODPolicy::ShouldSplit`; split into `GetChildren`'s 4 children if yes, otherwise
  append the node as a leaf. The walker owns an explicit `Node.Depth < MaxDepth` guard deliberately
  redundant with `ShouldSplit`'s own `CurrentDepth` check - `GetChildren` itself silently degrades at
  `MaxDepth` (returns non-advancing "children"), so relying solely on the policy function's internal
  check would risk infinite recursion if that check were ever weakened elsewhere; the tree walker
  itself must own this bound, not just delegate it. `ShouldMerge` is deliberately unused here - this
  is a stateless, always-fresh-from-the-roots walk with no resident state to decide collapsing
  against (that's item 5). This is a topological correctness claim (does the output leaf-set tile the
  whole sphere with no gaps/overlaps, and respect `MaxDepth`), not a tuned heuristic, so held to that
  bar: verified both by the implementing agent (5 scenarios spanning orbit altitude to standing-on-
  the-ground plus a forced-to-`MaxDepth` case, 20,000 random-direction coverage/overlap samples plus
  4,063 gathered leaves checked for nesting/`MaxDepth` compliance, zero failures, cross-checked
  against a second independently-coded oracle) and independently re-verified from scratch afterward
  (a fresh, not-reused Python reimplementation, different scenarios/random seed: 20,000 more coverage
  samples across 5 scenarios and 4,435 leaves, again zero gaps/overlaps/nested-pairs/`MaxDepth`
  violations, plus a 4-point monotonicity sweep with zero violations). This is the NAIVE,
  *unrestricted* walk only - two adjacent output leaves can differ by an arbitrary number of depth
  levels; capping that to 1 level is item 4, below.
- **`FSolarOrbzChunkRestrictedQuadtree::ApplyNeighborDepthRestriction`** (Phase 1, continued, item 4 -
  `SolarOrbzChunkRestrictedQuadtree.h/.cpp`, new files) - the standard restricted-quadtree fixpoint
  (used identically by CDLOD-style terrain LOD systems), and the actual reason `GetEdgeNeighbor` got
  built before any of this streaming-manager work started. Takes `GatherDesiredLeaves`'s naive,
  unrestricted output and force-splits whichever leaves are too shallow next to a deep same-depth
  edge-neighbor until every leaf's 3 edge-neighbors are within 1 depth level of its own. Algorithm: a
  `TSet<FSolarOrbzChunkAddress>` leaf set plus a FIFO worklist seeded with every leaf; for each popped
  leaf and each of its 3 edges, `GetEdgeNeighbor` gives the same-depth neighbor address, and
  `FindCoveringLeafDepth` (a new public helper - walks `GetParent()` upward checking set membership at
  each step) finds the depth of whichever leaf actually covers that neighbor address today; if that
  covering leaf is more than 1 level shallower than the chunk asking, it gets force-split into its 4
  children (regardless of what the LOD policy itself would have wanted there - the restriction can
  only ever request *more* detail than the raw heuristic, never less, which is the expected and
  accepted trade for this item), and both the new children and the original leaf are re-queued.
  Terminates because every split permanently reduces a bounded "depth debt" and `MaxDepth` caps how
  deep any single region can go. **Pentagon-vertex (5-valent) corners are deliberately NOT given a
  dedicated restriction pass** - only the 3 ordinary edges (`GetEdgeNeighbor`) are enforced, not
  `GetPentagonVertexNeighbors`'s corner-fan case; measured directly (not assumed) across several
  adversarial test inputs including a deliberately extreme 8-level forced mismatch at a pentagon
  vertex, edge-only restriction already keeps the worst same-point depth spread among the up-to-5
  wedges at any of the 12 original vertices to 2 levels, never more - judged not worth a dedicated
  pass given skirts (item 6) need to absorb a point discontinuity anyway and a point mismatch is a far
  smaller artifact than an edge-long crack. A topological/combinatorial correctness claim, held to the
  strict bar: verified by the implementing agent (4 adversarial/realistic naive leaf sets including a
  uniform one-face-vs-rest 6-level and 8-level mismatch, a 5-face checkerboard pattern, and a real
  `GatherDesiredLeaves`-derived input; 238,245 (leaf, edge) pairs checked for the neighbor-depth
  invariant with zero failures, a post-fixpoint tiling re-check with zero gaps/overlaps across 9,300
  samples plus a 34.9M-pair structural nesting check with zero violations, and zero `MaxDepth`
  overruns) and independently re-verified afterward using a genuinely different method - not a
  transcription of `GetEdgeNeighbor`'s path-ascend/descend algorithm, but a geometric
  nudge-across-the-edge-then-point-locate approach built on this project's already-verified point-
  location machinery - confirming naive inputs really do violate the invariant (264 of 9,216 checked
  pairs) and that the same fixpoint algorithm, independently implemented, drives it to zero violations
  (0 of 10,737 pairs) while preserving the tiling property (0 gaps/overlaps across 4,500 samples).

**Known, deliberately out of scope for this pass** (do not assume these are solved):

- **No neighbor/LOD-seam stitching yet.** `GetPentagonVertexNeighbors` and `GetEdgeNeighbor` above are
  both lookup primitives - nothing yet consumes either to actually stitch geometry. Two adjacent
  chunks at different quadtree depths will show a visible crack/T-junction where their edge vertex
  densities don't match - a well-known, well-solved problem (skirts, edge morphing, or simply never
  letting neighboring resident chunks differ by more than one LOD level), just not implemented here.
  Don't stream mixed-LOD neighbors in yet.
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
- **No streaming manager, no baking/Nanite path, no ASN_MK1 integration.** Point-location (item 1),
  the LOD policy (item 2), the unrestricted residency walk (item 3), and the restricted-quadtree
  fixpoint (item 4) above exist now, but items 5-7 (resident-set lifecycle, skirts, and the owning
  actor) don't yet - none of the pieces above are wired into anything that actually streams chunks in
  or out. Also note item 4's own deliberate scope limit: it enforces the 1-level restriction across
  ordinary edges only, not at the 12 pentagon vertices' corner-fan case (see that item's own write-up
  for the measured bound this rests on).
- **Triangulation winding is UNVERIFIED** - written without a compiler or renderer.
  `FSolarOrbzIcoSphereChunkGenerator::GenerateChunk`'s within-chunk grid triangulation applies the same
  "swap the last two corners" empirical fix `FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize`
  documents, by analogy rather than by independent verification. **Test a generated chunk's front/back
  facing before building anything else on top of it.**
