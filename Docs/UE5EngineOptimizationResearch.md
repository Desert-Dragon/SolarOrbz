# UE 5.8.3 Engine Optimization Research (Dragons-5.8.3 fork)

**Status: research only - nothing in this file has been implemented.** Requested directly, while
the `Desert-Dragon/UnrealEngine` fork (branch `Dragons-5.8.3`, confirmed via
`Engine/Build/Build.version`: Major 5 / Minor 8 / Patch 3) was attached to this session for the
Delete-key verification pass. This is a holding pen of candidate optimizations to the **engine
source/config itself** - not the SolarOrbz plugin - to triage and implement later, each picked for
relevance to what this project actually stresses: planet-scale chunked procedural mesh streaming
(`Docs/ChunkedPlanetTerrain.md`), heavy `ParallelFor` use, Large World Coordinates (double-precision
math at planetary radii), and an eventual MMO client/server split (`MMOBackend_Fork`).

Every CVar/default below marked **[grounded]** was read directly out of this exact `Dragons-5.8.3`
checkout (file path + line given) - not assumed from general UE knowledge, per this repo's own
UE5.8-specific verification standard. Items marked **[general]** are standard UE5 engine-optimization
practice not specifically re-checked against this fork's source this pass - worth a grounding pass
before acting on them, same bar as everything else in this plugin's docs.

## How to read this doc

Each item has:
- **What** - the mechanism/CVar/setting.
- **Why it matters here** - the specific thing about this project that makes it relevant, not a
  generic "this is good for all UE5 games" claim.
- **Where** - file/CVar name, so it's easy to find again without re-deriving.
- **Caution** - what could go wrong if flipped without testing.

Nothing here should be flipped in a shipping config without a profiling pass on the actual target
hardware - these are candidates to test, not a prescribed checklist.

## 1. Ray tracing BLAS residency/build budget — directly follows up this session's chunk RT fix

**What [grounded]:** `Engine/Source/Runtime/Engine/Private/Rendering/RayTracingGeometryManager.cpp`
declares the whole residency/eviction system this session's `bVisibleInRayTracing = false` fix
(`SolarOrbzChunkResidentSetManager.cpp`) worked around rather than tuned:

| CVar | Default | Line |
|---|---|---|
| `r.RayTracing.UseReferenceBasedResidency` | `true` | 52-67 |
| `r.RayTracing.ResidentGeometryMemoryPoolSizeInMB` | `400` | 77-84 |
| `r.RayTracing.NumAlwaysResidentLODs` | `1` | 97-113 |
| `r.RayTracing.Geometry.MaxBuiltPrimitivesPerFrame` | `-1` (unlimited - builds immediately) | 129-135 |
| `r.RayTracing.Geometry.PendingBuildPriorityBoostPerFrame` | `0.001f` | 137-143 |

**Why it matters here:** `FSolarOrbzChunkResidentSetManager::UpdateResidentSet` has no per-update
spawn/despawn budget (flagged as a known gap in its own header, and again in `ROADMAP.md`'s
ray-tracing-fix entry) - a large viewer jump can spawn/destroy many chunk components in one
`UpdateResidentSet` call. `bVisibleInRayTracing = false` sidesteps the eviction `ensure()` entirely
by opting chunks out of RT, which is correct for now (no RT reflections/shadows are wired into
chunk rendering yet), but it's a workaround, not a tuned budget. If/when chunks need to participate
in ray tracing (reflections on terrain, say), `r.RayTracing.Geometry.MaxBuiltPrimitivesPerFrame`
(currently unlimited) is the actual frame-time throttle for BLAS build bursts, and
`ResidentGeometryMemoryPoolSizeInMB`/`NumAlwaysResidentLODs` govern how much churn the eviction
pool tolerates before thrashing. Tuning these together with the resident-set manager's own future
spawn budget (the gap already on record) is the real fix, not a config-only one.

**Caution:** `MaxBuiltPrimitivesPerFrame` trades frame-time smoothness for build latency - setting
it too low means newly-streamed-in chunks visibly "pop in" to ray tracing a few frames late.

## 2. Incremental Garbage Collection — off by default, directly relevant to chunk churn

**What [grounded]:** `Engine/Source/Runtime/CoreUObject/Private/UObject/GarbageCollection.cpp`:

| CVar | Default | Line | Note |
|---|---|---|---|
| `gc.IncrementalBeginDestroyEnabled` | `1` (on) | 230-236 | Spreads `BeginDestroy` over frames |
| `gc.IncrementalBeginDestroyGranularity` | `10` | 238-244 | Destroys-per-time-check |
| `gc.AllowIncrementalReachability` | `0` (off, "experimental") | 302-308 | Marked experimental |
| `gc.IncrementalReachabilityTimeLimit` | `0.005f` (5ms) | 310-316 | Only applies if enabled above |
| `gc.AllowIncrementalGather` | `0` (off, "experimental") | 318-324 | Marked experimental |

**Why it matters here:** every `UpdateResidentSet` spawn/despawn cycle creates and destroys
`UProceduralMeshComponent`s and, per chunk, a full `FSolarOrbzIcoSphereMeshData` worth of transient
allocations; a fast-moving viewer over planet-scale chunked terrain is close to a worst case for
GC churn. Incremental `BeginDestroy` is already on by default, which helps, but full reachability
analysis/gather (the expensive full-GC passes) are both off by default and marked experimental -
worth a controlled test (not a blind flip, given the "experimental" label) once there's a real
profiling target (PIE flying over a generated Earth-scale planet is exactly the right test case,
and this plugin already has the Chunked Planet Preview to produce one without entering Play).

**Caution:** both incremental-reachability CVars are marked experimental in Epic's own comment -
test for correctness (missed collections, stale object lifetime) before trusting them, not just
frame time.

## 3. TaskGraph worker thread tuning — ParallelFor is already this plugin's main lever

**What [grounded]:** `Engine/Source/Runtime/Core/Private/Async/TaskGraph.cpp`:

| CVar | Default | Line | Note |
|---|---|---|---|
| `TaskGraph.NumForegroundWorkers` | `2` | 131-138 | `CORE_API int32 GNumForegroundWorkers` |
| `TaskGraph.OversubscriptionRatio` | present, see file | 114-124 | Controls thread oversubscription |
| `TaskGraph.UseDynamicThreadCreation` | present, see file | 125-130 | |
| `TaskGraph.UseBackgroundThreads` | present, see file | 184-191 | |

**Why it matters here:** this plugin already leans hard on `ParallelFor` - whole-sphere Pass A/B
(`SolarOrbzIcoSphere.cpp`), and the chunked resident-set manager's own Phase 1
(`FSolarOrbzChunkResidentSetManager::UpdateResidentSet`, `GenerateChunk` + `AppendSkirts` per
spawning chunk). All of that work goes through the engine's shared TaskGraph thread pool, which
every other engine subsystem (rendering, audio, async loading) also competes for. Worth profiling
whether the default foreground-worker count leaves enough headroom during a big resident-set
update, or whether a background-priority dispatch (rather than default `ParallelFor` priority)
would reduce contention with render-thread-adjacent work during a streaming burst.

**Caution:** this is a global engine setting, not a per-call-site one - changing it affects every
other subsystem's threading too, not just this plugin's own `ParallelFor` calls.

## 4. Nanite for planet terrain — feasibility question, not a CVar flip

**What [general]:** Nanite (`r.Nanite.*` CVars) virtualizes geometry so LOD/draw-call cost stops
scaling with triangle count the way traditional LODs do.

**Why it matters here:** the entire reason `AASolarOrbzChunkedPlanetActor` exists is that a single
mesh can't carry ground-level detail at planetary radius (`ASolarOrbzIcoSphereActor`'s own
`RadiusMeters` comment: subdivision level 23, ~700 trillion vertices, "not achievable in any single
mesh"). Nanite's whole value proposition is sidestepping exactly that kind of LOD/triangle-budget
problem. **However**: Nanite meshes are built from static, baked geometry (via `UStaticMesh`
Nanite data, built at cook/import time) - this plugin's chunks are runtime-procedural
(`UProceduralMeshComponent`, regenerated per-chunk at stream-in time from noise/layer evaluation),
which is fundamentally the opposite shape of what Nanite expects. This is a real, substantial
design question (would need either baking each chunk to a Nanite-enabled static mesh at generation
time - adding cook/build latency per chunk - or UE5's more experimental runtime-Nanite paths), not
a config change, and worth a dedicated design doc of its own before attempting, not a bullet point
to implement blind.

**Caution:** this is the single biggest-effort item in this file by a wide margin - flagged here as
"worth researching further," not "worth doing."

## 5. World Partition / HLOD — an alternative streaming backbone, not a drop-in

**What [general]:** UE5's World Partition system (grid-based level streaming, HLOD generation for
distant cells) is Epic's own answer to "how do I stream a huge world without the designer managing
sublevels by hand."

**Why it matters here:** this plugin already built its own bespoke planet-chunk streaming system
(icosphere-based quadtree addressing, point-location, LOD policy, restricted-quadtree fixpoint,
resident-set diffing - the whole `Docs/ChunkedPlanetTerrain.md` design) specifically because a
*spherical* world doesn't map onto World Partition's planar grid model. World Partition is built
for planar/open-world terrain (Landscape-shaped), not a sphere addressed by icosahedral
quadtree faces. Not a replacement candidate for the existing system - but HLOD generation
*techniques* (distance-based mesh simplification/merging for far chunks) could be worth borrowing
conceptually for a future LOD-blending pass on far-from-viewer chunks, without adopting World
Partition itself.

**Caution:** this is included mainly to head off the obvious "why not just use World Partition"
question - the short answer is the sphere-vs-plane mismatch, not an oversight.

## 6. Large World Coordinates (LWC) — double-precision cost is inherent, not a bug to fix

**What [general]:** UE5's `FVector`/`FTransform`/etc. use `double` by default (confirmed earlier
this session: `sizeof(FVector) == 24`, used to diagnose the `TArray` self-aliasing crash in
`SolarOrbzChunkSkirtBuilder.cpp`). Double-precision math is inherently ~2x the cost of float math on
most hardware for the same operation count.

**Why it matters here:** this plugin's entire value proposition is planet-scale radii (Earth's
~6,371,000m) with meter-scale detail - exactly the scenario LWC exists for, and exactly the
scenario that pays LWC's double-math cost the most (every vertex position, every noise sample
point, every layer evaluation runs through double-precision `FVector` math in
`SolarOrbzNoiseBasis`/`SolarOrbzTerrainLayers.cpp`). This is *why* LWC exists as a feature, not a
regression to fix - but it's worth knowing the render-side mitigation UE5 already does: vertex
buffers are stored as `float`/`FVector3f` relative to a per-actor/per-component double-precision
origin, so the double-math cost is concentrated in CPU-side generation (exactly this plugin's own
`GenerateChunk`/noise evaluation), not GPU rendering. No specific CVar to flip here - this is a
"know the cost model" item, relevant if a future profiling pass shows chunk generation CPU-bound.

**Caution:** none to flip - this is background knowledge for interpreting a future profile, not an
action item.

## 7. Memory allocator (mimalloc vs. default binned allocator)

**What [general]:** UE5 supports swapping the default `FMalloc` implementation
(`FMallocBinned3`/platform default) for `mimalloc` on supported platforms via
`-mallocmimalloc`/project config, generally reducing allocation/free overhead and fragmentation
under heavy alloc churn.

**Why it matters here:** every chunk spawn/despawn cycle allocates/frees a full
`FSolarOrbzIcoSphereMeshData` (Vertices/Normals/Tangents/UVs/Triangles arrays) per chunk, on top of
whatever the engine itself is already churning - exactly the "heavy allocation churn" scenario
mimalloc is usually recommended for. **Not yet grounded against this exact fork's build** (a quick
grep for `MallocMimalloc`/`FORCE_ANSI_ALLOCATOR` in this checkout came back empty under the paths
checked - worth a proper search pass, not concluding either way from one miss, before treating this
as confirmed available/default in `Dragons-5.8.3`).

**Caution:** allocator swaps are typically a build-time/platform config choice (and sometimes a
licensing/packaging consideration depending on platform), not a runtime CVar - needs its own
grounding pass against this fork before scoping as an actual task.

## 8. Build/iteration velocity — not runtime perf, but directly affects this team's dev speed

**What [general]:** Unity build batching, Live Coding (hot-patching running processes without a
full relink), and Zen Server/DDC (Derived Data Cache) for faster cook/PIE-launch iteration are all
standard UE5 build-speed levers, separate from runtime game performance.

**Why it matters here:** this plugin has been under heavy active iteration this session alone (new
layer algorithms, graph editors, chunked streaming, multiple bug-fix cycles) - build/iteration
speed is a real productivity cost distinct from the shipped game's runtime performance, and worth
its own look (project `Build.cs`/`Target.cs` Unity settings, whether Zen/DDC is already configured
for this team) separately from the runtime-focused items above.

**Caution:** lowest-risk category here (doesn't affect shipped behavior at all), but also the one
most dependent on this team's actual local/CI setup, which this research pass didn't inspect.

## Suggested triage order (my own read, not a decision)

1. **#1 (ray tracing budget)** and **#2 (incremental GC)** - cheapest to test (pure CVar/config,
   already have the Chunked Planet Preview as a built-in profiling scenario), most directly tied to
   a gap this session already found and partially worked around.
2. **#3 (TaskGraph tuning)** and **#7 (mimalloc)** - needs real profiling data first (which worker
   pool is actually contended, which allocator is actually in use) before picking a specific change.
3. **#8 (build velocity)** - independent of the above, worth a quick audit regardless since it's
   zero-risk to runtime behavior.
4. **#4 (Nanite)** and **#5 (World Partition)** - biggest effort/research-before-design items;
   treat as "worth a dedicated design doc later," not near-term work.
5. **#6 (LWC cost model)** - no action item, just useful context for interpreting whatever profiling
   comes out of #1-#3.

---
*Branch `claude/fervent-bohr-q1tlol`. Engine source checked against `Desert-Dragon/UnrealEngine`,
branch `Dragons-5.8.3` (local checkout: `/home/user/unrealengine` in the session that wrote this).*
