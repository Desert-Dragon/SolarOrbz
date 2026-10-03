# Chunked Planet Terrain — First Playtest Guide

This is a practical runbook for the **first time a human presses Play** on
`AASolarOrbzChunkedPlanetActor`. It assumes the plugin now compiles (it does, as of
`e5bfd66`) but has never been run — every piece up to this point was verified either
with Python ground-truth scripts (outside the engine) or by careful code review (no
compiler was available while any of it was written). That is a fundamentally weaker
guarantee than "tested," so **treat this first session as debugging, not a
showcase.** Finding something wrong here is the expected, normal outcome, not a sign
anything was done carelessly — see `Docs/ChunkedPlanetTerrain.md`'s own closing notes
on this.

This doc is the *how to test it* companion to `Docs/ChunkedPlanetTerrain.md` (the
*how it works* design doc). Read that one first if you want the algorithms; this one
assumes you just want chunks on screen as fast as possible, in an order that
localizes bugs instead of throwing everything at once.

---

## 0. Before you touch the editor

A few things worth knowing going in, so a weird result doesn't read as scarier than
it is:

- **Collision is off** on every streamed chunk. You will fall through the planet if
  you try to stand on it. This is a known, documented gap (item 5), not a bug.
- **Biome colors, climate, and gravity/atmosphere (Profile) are not wired up.**
  `BiomeStack`/`ClimateSimulation`/`Profile` exist on the actor for property parity
  with `ASolarOrbzIcoSphereActor` but are not read by anything yet. Expect a flat,
  unlit-looking surface unless your `Material` does something interesting with
  normals/UVs alone.
- **Triangle winding is explicitly unverified.** Both `GenerateChunk`'s own
  triangulation and the skirt builder's triangulation were written "by analogy" to
  the whole-sphere generator's empirical fix, never confirmed against a real
  renderer. If the planet looks inside-out (you can see through the front faces, or
  the surface is dark/backface-culled from outside), this is the first thing to
  suspect — see the troubleshooting table.
- **The `Update Chunks Now` editor button only does something while actually
  Playing**, not in the static level-editor view. `ResidentSetManager` is
  constructed in `BeginPlay`, so the button is a no-op (silently — it just returns
  early) until you've pressed Play at least once in this session.

---

## 1. Smallest possible first test (do this before anything else)

Goal: confirm chunks spawn and render at all, with the fewest moving parts.

1. Open (or create) a test level.
2. Drag an `AASolarOrbzChunkedPlanetActor` into the level.
3. Set these properties on it:
   - `RadiusMeters` → something small and human-scale, e.g. **500** (not
     6,371,000 — a planet-scale radius makes the very first look-and-feel pass
     harder to read, see §3 for scaling up later).
   - `TerrainStack` → **leave empty**. `nullptr` makes `GenerateChunk` produce a
     plain undisplaced sphere patch — the simplest possible shape to sanity-check
     against before adding real terrain into the mix.
   - `Material` → any simple opaque material (even the engine default checker
     pattern is fine — this is about geometry, not looks, yet).
   - `ChunkResolution` → leave at the default (**16**).
   - `bUseViewerWorldPositionOverride` → **true**.
   - `ViewerWorldPositionOverride` → a point just above the surface, e.g.
     `(0, 0, 50100)` for a 500 m (50,000 cm) radius — that's ~1 m above the ground
     along the Z axis. (Reminder: this is in UE units/cm, relative to the planet's
     own center, same convention as `GatherDesiredLeaves`.)
   - Leave `ViewerActor` unset.
   - Leave `LODSettings`, `SkirtDepth` (100 cm = 1 m), `UpdateIntervalSeconds` (1.0s)
     at their defaults for this first pass.
4. Press Play.
5. Check the Output Log for:
   - `LogSolarOrbzChunk` lines (`"generated BaseFace=... Depth=... PathBits=...,
     %d verts, %d tris"`) — one per spawned chunk. If you see these, mesh
     generation is at least running.
   - No `LogSolarOrbzChunkedPlanet` warning about a missing viewer (that warning
     means the override/ViewerActor setup above didn't take effect as expected).
6. Look at the viewport. You should see a patch of sphere surface near the
   override position, built out of several visibly different-sized triangular
   chunks (finer near the override point, coarser farther away) — not one smooth
   continuous mesh, and not the *whole* sphere (this planet is radius 500 m; most
   of its surface is far from the one viewer point you set, so most of it should
   stay shallow/coarse, which is correct, not a bug).

If nothing renders at all, see the troubleshooting table below before changing
anything else — don't start tuning `SkirtDepth`/`LODSettings` yet; get something
visible first.

---

## 2. Second pass: confirm the LOD/streaming behavior actually streams

Still with the small test radius from §1:

1. In the Details panel, drag `ViewerWorldPositionOverride` closer to and farther
   from the surface while in Play (or bind it to a key/Blueprint if that's easier)
   — watch chunks subdivide as it gets closer, and merge back as it retreats.
   `UpdateIntervalSeconds` defaults to 1 second, so give it a beat after each move.
2. Switch to a moving viewer instead of a static override:
   - Set `bUseViewerWorldPositionOverride` back to **false**.
   - Set `ViewerActor` to your pawn/camera (whatever you're controlling in PIE).
   - Fly/walk around the planet and confirm chunks keep resolving around wherever
     you currently are, not around the old override point.
3. Watch for **thrashing** right at a split/merge boundary — a chunk rapidly
   splitting and re-merging every update tick while you're roughly stationary.
   A SMALL amount of back-and-forth near the boundary is an acknowledged,
   documented gap (`FSolarOrbzChunkLODPolicy`'s own header calls this out:
   hysteresis stops *flicker at a fixed position* but not *sustained oscillation
   exactly across the dead zone over many updates*) — a chunk settling within a
   update or two is fine. Continuous visible popping that never settles is a real
   bug, not the known gap.

---

## 3. Scaling toward a real planet

Once §1–2 look right at toy scale:

1. Raise `RadiusMeters` in stages rather than jumping straight to Earth scale —
   try 10,000 (10 km), then 1,000,000 (1,000 km), then 6,371,000 (Earth) —
   watching chunk counts and frame time at each step. `GetResidentChunkCount()`
   (Blueprint-callable) is the quickest way to see how many components exist
   without counting them in the viewport.
2. At larger radii, a single `UpdateResidentSet` call (triggered by the timer) can
   generate many more chunks per newly-needed set. This runs synchronously on the
   game thread when the timer fires (mesh generation itself is parallelized via
   `ParallelFor` inside the resident-set manager, but the call as a whole still
   blocks until that finishes) — if you see a frame hitch exactly on the tick
   where a lot of new chunks appear, that's expected per the design doc's own
   "known limitations" list (no frame-spread/async dispatch yet), not a surprise
   bug. If it's bad enough to matter, raising `UpdateIntervalSeconds` is the
   cheapest mitigation for now.
3. Re-introduce a real `TerrainStack` once undisplaced chunks look right —
   confirm displaced terrain still streams/merges sensibly, and specifically look
   at whether seams at LOD boundaries get *worse* with real height variation
   (skirts hide a flat crack more reliably than a tall one — if a cliff-height
   mismatch pokes through the skirt, `SkirtDepth` likely needs to be bigger than
   the 1 m placeholder, or scaled relative to local terrain amplitude — not
   attempted yet, see the design doc's own "flat, non-distance-aware `SkirtDepth`"
   gap).

---

## 4. Troubleshooting

| Symptom | Likely cause | What to check |
|---|---|---|
| Nothing renders at all, no log lines | `BeginPlay` never ran (actor not in a loaded level / not in PIE), or the actor's `RootComponent` is null | Confirm you pressed Play, not just viewing the editor; confirm the actor exists in the level you're playing |
| `LogSolarOrbzChunk` lines appear but nothing visible | `Material` unset and the engine default material isn't rendering the way you expect, or the mesh is there but winding-culled away from your camera angle | Try orbiting the camera to the opposite side; try a material with **Two Sided** enabled as a diagnostic (not a fix) to rule out culling |
| Whole patch looks inside-out / dark from outside, lit from inside | Triangle winding — explicitly unverified in both `GenerateChunk` and the skirt builder | Toggle the material's Two Sided flag as a diagnostic; if that "fixes" it, winding needs an actual fix (see `Docs/ChunkedPlanetTerrain.md`'s triangulation-winding note) — report this back, don't just ship Two-Sided as the permanent fix |
| `LogSolarOrbzChunkedPlanet` warning: "no ViewerActor and bUseViewerWorldPositionOverride is false" | Viewer not actually configured despite §1's steps | Re-check `bUseViewerWorldPositionOverride` is true AND `ViewerActor` is empty (ViewerActor takes priority when both are set) |
| Only ONE giant shallow chunk ever appears, never splits | LOD ratio never crosses `SplitScreenSizeRatio` (default 1.0) — usually means `ViewerWorldPositionOverride`/`ViewerActor` position is far from the surface relative to `RadiusMeters`, or units got mixed up (meters vs. cm) | Double check `ViewerWorldPositionOverride` is in **cm**, relative to the planet's **center**, not "1 meter above the ground" typed as `1.0` |
| Visible cracks/gaps at chunk boundaries even with skirts | `SkirtDepth` too shallow for the actual depth mismatch at that edge, or terrain displacement amplitude exceeds the skirt | Increase `SkirtDepth`; if using a `TerrainStack`, try testing without one first (§1) to isolate whether this is a skirt-sizing issue or a terrain-displacement issue |
| A chunk visibly flickers between split/merged state continuously, never settling | Possible real bug in `ApplyNeighborDepthRestriction`'s fixpoint, or the viewer sitting exactly on a split/merge boundary in a way the hysteresis gap doesn't cover | Move the viewer decisively away from that spot and see if it stops; if it keeps happening at many different positions, that's a real bug worth reporting with the viewer position and radius |
| Frame hitches on the tick new chunks appear, otherwise smooth | Expected — `UpdateResidentSet` runs synchronously, no frame-budget spreading yet | Raise `UpdateIntervalSeconds`, or reduce `ChunkResolution`, as a stopgap |
| Editor "Update Chunks Now" button does nothing | Not currently in Play — `ResidentSetManager` only exists between `BeginPlay` and `EndPlay` | Press Play first, then use the button (or just wait for the timer) |
| Fell through the planet | Collision is deliberately not built for streamed chunks yet (item 5's own gap) | Not a bug — use a separate collision volume/the whole-sphere actor if you need to stand on something right now |

---

## 5. What to report back

If something looks wrong and isn't in the table above, the fastest way to get it
fixed is to include:

- `RadiusMeters`, `ChunkResolution`, `SkirtDepth`, whether `TerrainStack` was set.
- The viewer setup (override position, or which actor) at the moment you saw the
  issue.
- Whatever `LogSolarOrbzChunk*` lines were in the log around that time.
- A screenshot if it's visual (winding/seams/popping are much faster to diagnose
  from a picture than a description).

That's usually enough to reproduce and fix without a back-and-forth round of
"what were your settings."

---

## 6. What this guide deliberately does not cover

- Tuning `LODSettings`/`SkirtDepth` for a *good-looking* result — both are
  first-pass placeholders per their own header comments, not defaults expected to
  survive contact with a real camera. This guide is about confirming the system
  *works*, not about making it look finished.
- Anything already listed as explicitly out of scope in
  `Docs/ChunkedPlanetTerrain.md` (real vertex-stitching instead of skirts, spawn
  budgeting for a teleport-sized viewer jump, antimeridian handling, baking/Nanite,
  ASN_MK1 gravity/atmosphere integration). None of that is expected to work yet,
  and none of it is this guide's concern.
