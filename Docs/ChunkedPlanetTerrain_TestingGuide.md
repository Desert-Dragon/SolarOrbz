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
assumes you just want chunks on screen as fast as possible, with purpose-built tools
to actually see what's wrong when something is, not just a list of settings.

**Structure:** §1 is what to *build* (a test level and a handful of small, disposable
editor assets — a Blueprint, a diagnostic material, a debug pawn) before you touch
any gameplay logic. §2 is the exact property reference — every tunable, where it
lives, and what to set it to. §3 is a staged test matrix with explicit pass/fail
criteria per stage. §4 covers Unreal's own built-in diagnostics worth knowing about.
§5 is troubleshooting. §6 is what to report back.

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
  renderer. §1.3 below builds a material specifically to make this visible and
  precisely localized instead of just "looks dark from outside."
- **The `Update Chunks Now` editor button only does something while actually
  Playing**, not in the static level-editor view. `ResidentSetManager` is
  constructed in `BeginPlay`, so the button is a no-op (silently — it just returns
  early) until you've pressed Play at least once in this session.
- **FVector is double-precision in UE5 (Large World Coordinates).** Planetary-scale
  positions (millions of UE units from the origin) are not the precision landmine
  they'd have been in UE4 — worth knowing so you don't spend time suspecting float
  precision for something that's actually one of the issues below.

---

## 1. Build the test harness

Do this once, up front. None of this is throwaway work — it's the actual tooling
you'll use for every stage in §3, and it's small enough to build in one sitting.

### 1.1 Test level

Create a new, empty level (`File → New Level → Empty Level`, or duplicate an
existing empty one) named something like `L_ChunkedPlanetTest`. Keep it separate
from any real game level — you'll be aggressively changing properties, flying
through geometry, and possibly crashing PIE while a real bug is being chased, and
none of that should touch content anyone else is relying on.

Add a `DirectionalLight` and a `SkyLight` (or just drag in a copy of the engine's
default `BP_Sky_Sphere` + lighting setup) so an Unlit-vs-Lit material actually shows
something — this matters once you get to §1.3's diagnostic material, and generally
makes screenshots you might need to share (§6) actually legible.

### 1.2 A Blueprint subclass of the actor

Right-click in the Content Browser → `Blueprint Class` → search for
`AASolarOrbzChunkedPlanetActor` as the parent → name it `BP_ChunkedPlanetTest`.

Why a Blueprint and not just placing the C++ class directly: every property on this
actor is `EditAnywhere`, so you *can* configure a raw placed instance directly in the
level's Details panel — but a Blueprint subclass gives you **Class Defaults** you can
tweak once and have every level instance inherit, and room to add one small,
test-only convenience without touching C++:

- A **Timeline or simple Tick** moving `ViewerWorldPositionOverride` smoothly toward
  and away from the surface, if you want a repeatable "approach and retreat" motion
  for §3.2 without having to manually drag a vector field every few seconds. (Not
  required — manual dragging works fine too — but handy if you end up re-running
  this stage a lot.)

The manual "force an update right now" control lives on the debug *pawn* instead
(§1.4) — only Pawns/PlayerControllers receive input by default in UE5.8's Enhanced
Input system, and this actor isn't a Pawn, so binding input directly on it would mean
extra, unnecessary setup (`Auto Receive Input` plus its own Input Mapping Context)
for a control the pawn can already provide by calling across to this actor.

Drag one instance of `BP_ChunkedPlanetTest` into `L_ChunkedPlanetTest`. This is the
instance you'll be editing throughout §2–§3.

### 1.3 A winding-diagnostic material

This is the single most useful thing to build before starting, because "triangle
winding is unverified" is explicitly called out as the top risk in this subsystem,
and guessing from how dark something looks is a much weaker signal than seeing it
directly.

Create a new Material, `M_SolarOrbz_WindingDebug`:

1. Open it, and in the **Details** panel of the Material Editor:
   - **Shading Model** → `Unlit` (removes lighting as a confound — you want to see
     winding, not shadows).
   - **Two Sided** → `true` (so backfaces still render instead of being culled —
     you want to see them colored differently, not invisible).
2. In the graph, add a **`Two Sided Sign`** expression node (search for it in the
   node palette — it's a built-in Unreal material expression that outputs `1.0` for
   a face Unreal is rendering as "front" and `-1.0` for one it's rendering as
   "back," which is exactly the frontface/backface distinction winding controls).
3. Remap that from `[-1, 1]` to `[0, 1]` with a **`Remap Value Range`** node
   (`UMaterialExpressionRemapValueRange`, present in UE5.8's node palette — search
   "Remap"): `Input Range Min` = `-1`, `Input Range Max` = `1`, `Target Range Min`
   = `0`, `Target Range Max` = `1`. (Equivalent to chaining `Add` 1.0 then
   `Multiply` 0.5 by hand, or just feeding `Two Sided Sign` straight into a `Lerp`'s
   Alpha and letting Unreal's own clamping sort it out — `Remap Value Range` is the
   single node that does it explicitly, so use that one.)
4. Feed that into a `Lerp` between two `Constant3Vector` colors — green (e.g.
   `(0, 1, 0)`) for Alpha = 1 (front), red (e.g. `(1, 0, 0)`) for Alpha = 0 (back).
5. Plug the `Lerp` output into **Emissive Color**.
6. Save and compile.

Assign `M_SolarOrbz_WindingDebug` to `BP_ChunkedPlanetTest`'s `Material` property for
every test stage in §3 up through the point where you're satisfied winding is
correct — swap to a real material only afterward. **Expected correct result: the
entire visible planet surface renders green from any outside viewing angle, with no
red patches.** Any red patch is a winding bug, and because the color is applied per-
triangle, a red patch tells you exactly which chunk(s) are wrong rather than a vague
"it looks inside-out" — screenshot it and that screenshot alone is most of a bug
report (see §6).

### 1.4 A debug pawn with on-screen info and manual controls

Create a Blueprint subclass of `SpectatorPawn` (or `DefaultPawn`, either is fine —
`SpectatorPawn` has no collision, which is convenient given §0's reminder that the
planet itself has none yet either) named `BP_ChunkDebugPawn`.

In its Event Graph, exact nodes and pins (UE5.8 uses Enhanced Input by default — see
§1.5 for the Input Action/Mapping Context assets this references):

1. **`Event BeginPlay`** does two things, chained one after the other (order between
   them doesn't matter, they don't depend on each other):
   - **Activate the input mapping** (needed once, so `Started`/`Triggered` events
     fire at all from the mapping context built in §1.5): `Get Controller` (pure,
     `Target` implicit self) → `Cast To PlayerController` (`Object` = `Get Controller`'s
     output; exec continues on success, `Cast Failed` otherwise) → `Get Local Player`
     (pure, `Target` = `As Player Controller`) → `Get Enhanced Input Local Player
     Subsystem` (pure, `Target` = `Get Local Player`'s output) → **`Add Mapping
     Context`** (exec node — `Target` = the subsystem, `Mapping Context` =
     `IMC_ChunkDebug`, `Priority` = `0`).
   - **Locate the planet**: `Get All Actors Of Class` (exec node, `Actor Class` =
     `BP_ChunkedPlanetTest`, output `Out Actors` — an array) → `Get` (pure array
     node, `Index` = `0`, takes `Out Actors`, outputs the first `Actor`) →
     `Cast To BP_ChunkedPlanetTest` (`Object` = that `Get` node's output; exec
     continues on success) → `Set TargetPlanet` (a new Object Reference variable,
     type `AASolarOrbzChunkedPlanetActor` — value = `As BP Chunked Planet Test`).
     This is how the pawn finds "the" planet in a test level that should only ever
     have one.
2. **`Event Tick`** (or, to reduce on-screen spam, a `Retriggerable Delay` loop —
   `In`/`Duration` in, `Completed` exec out, wired back into its own `In` to repeat
   every 0.25–0.5s): `Get Resident Chunk Count` (exec node, `Target` = `TargetPlanet`,
   output `Return Value` — an `Int`) → `Print String` (`In String` = that `Return
   Value` — UE4.27+/5.x auto-inserts a `Conv_IntToString` conversion node the moment
   you drag the `Int` pin onto `Print String`'s `FString` input, so you don't build
   that conversion by hand; `Print to Screen` = `true`, `Duration` ≈ the tick/delay
   interval so it doesn't visibly flicker) → `Get Actor Location` (exec node,
   `Target` implicit self, output `Return Value` — a `Vector`) → a second
   `Print String` (`In String` = that `Vector`, same auto-inserted
   `Conv_VectorToString`). Correlating "where am I" with "how many chunks exist
   right now" is the fastest way to notice when streaming has stopped reacting to
   movement.
3. **`IA_ForceChunkUpdate`** (an `Enhanced Input Action Event` node, `Action` =
   the `IA_ForceChunkUpdate` asset from §1.5) → its `Started` exec output (fires
   once on press, unlike `Triggered` which fires every frame held) →
   `Update Chunks Now` (exec node, `Target` = `TargetPlanet`) — forces an immediate
   recompute without waiting on `UpdateIntervalSeconds`, especially useful while
   deliberately holding still at a boundary for §3.2's thrashing check.
4. Optional but worth the five minutes: two more Input Actions (e.g.
   `IA_NudgeViewerIn`/`IA_NudgeViewerOut`) whose `Started` pins each feed
   `Get TargetPlanet` → `Get Viewer World Position Override` (pure getter) →
   a vector `+`/`-` node adding a small fixed offset → `Set Viewer World Position
   Override` (`Target` = `TargetPlanet`, only meaningful while
   `bUseViewerWorldPositionOverride` is true) — gives you precise, repeatable
   single-step control for straddling a split/merge boundary in §3.2, instead of
   trying to hover a mouse-dragged flying camera exactly on a knife edge.

Set `L_ChunkedPlanetTest`'s World Settings → `Default Pawn Class` to
`BP_ChunkDebugPawn` (or just manually possess one placed in the level) so PIE drops
you into it automatically.

### 1.5 Input bindings (Enhanced Input — UE5.8's default, not the legacy Action Mappings system)

UE5.8 projects use the Enhanced Input plugin by default, not the older
`Project Settings → Engine → Input → Action Mappings` list — that legacy system
still technically works, but Enhanced Input is what new projects actually use, so
that's what §1.4's graphs are built against. Two new assets, both plain
right-click-in-Content-Browser creates:

1. **`IA_ForceChunkUpdate`** — right-click → `Input` → `Input Action`. Leave
   `Value Type` at its default (`Digital (bool)` — a plain pressed/released signal
   is all this needs).
2. If you added §1.4 step 4's optional nudge controls, two more the same way:
   `IA_NudgeViewerIn` / `IA_NudgeViewerOut` (also `Digital (bool)`).
3. **`IMC_ChunkDebug`** — right-click → `Input` → `Input Mapping Context`. Open it,
   add a mapping for each Input Action above to a key (e.g. `IA_ForceChunkUpdate` →
   `F5`).
4. This is the `Mapping Context` §1.4 step 1's `Add Mapping Context` node activates
   at `BeginPlay` — without that call, the keys you bound here do nothing, since
   creating the mapping context asset alone doesn't register it with anything.

(If your project already has a fly-camera speed control bound via the default
`SpectatorPawn` behavior — scroll wheel or comma/period cycles through its built-in
speed steps — you don't need to rebuild that; it's stock engine behavior on
`SpectatorPawn` already, mentioned here only so you don't duplicate it.)

---

## 2. Property reference — what to change, where, and why

Every tunable on `AASolarOrbzChunkedPlanetActor`, grouped the way the Details panel
groups them (`SolarOrbz|ChunkedPlanet|...` categories), with what to actually set
during testing and why.

| Property | Category | Default | What to test with | Why |
|---|---|---|---|---|
| `RadiusMeters` | `ChunkedPlanet` | 6,371,000 (Earth) | **500** for §3.1–3.4, then stage up per §3.5 | Earth scale on the very first test makes everything (chunk count, how close "near the surface" is, how fast you need to fly) harder to reason about. Small first. |
| `TerrainStack` | `ChunkedPlanet\|Terrain` | unset | **leave unset** until §3.6 | Isolates mesh-generation/streaming bugs from terrain-displacement bugs. An undisplaced sphere patch is the simplest possible shape to sanity-check against. |
| `Material` | `ChunkedPlanet\|Terrain` | unset | `M_SolarOrbz_WindingDebug` (§1.3) until winding is confirmed correct, then any real material | See §1.3. |
| `BiomeStack` / `ClimateSimulation` / `Profile` | `ChunkedPlanet\|Biome` / `\|Climate` / `\|Profile` | unset | **leave unset** | Not read by anything yet (§0) — setting them does nothing observable right now; don't spend time on them in this pass. |
| `ChunkResolution` | `ChunkedPlanet\|Chunking` | 16 | leave at default for §3.1–3.5; try 8 and 32 once something works, to see the cost/quality tradeoff | Higher = denser per-chunk mesh, more vertices generated per spawn, same topology. Not expected to change correctness, only density/performance. |
| `LODSettings.SplitScreenSizeRatio` | `ChunkedPlanet\|Chunking` | 1.0 | leave at default initially | Lower = splits sooner/deeper (more detail, more chunks resident). First-pass constant per its own header comment — don't tune for *looks* yet (§3's whole point is correctness first). |
| `LODSettings.MergeScreenSizeRatio` | `ChunkedPlanet\|Chunking` | 0.75 | leave at default; must stay below `SplitScreenSizeRatio` | The hysteresis gap between the two is what stops flicker at a boundary (§3.2). Setting this ≥ `SplitScreenSizeRatio` removes that gap entirely — don't do that while testing for thrashing, you'd be removing the very thing you're checking for. |
| `SkirtDepth` | `ChunkedPlanet\|Chunking` | 100 (1 m, in cm) | default for §3.1–3.5; raise substantially (try 1000+) if §3.6's terrain test shows gaps poking through | Flat, non-distance-aware placeholder (its own header says so) — expect to need a bigger value once real terrain height variation is involved. |
| `UpdateIntervalSeconds` | `ChunkedPlanet\|Chunking` | 1.0 | leave at default; lower temporarily (e.g. 0.2) if you want faster feedback while deliberately testing streaming reactions in §3.2 | Lower = more responsive streaming, more frequent `UpdateResidentSet` calls (more frequent cost, see §3.5). |
| `ViewerActor` | `ChunkedPlanet\|Viewer` | unset | your `BP_ChunkDebugPawn` instance, once you move to §3.2's moving-viewer step | Takes priority over the override below when both are set — see the fallback order in the actor's own header comment. |
| `bUseViewerWorldPositionOverride` | `ChunkedPlanet\|Viewer` | false | **true** for §3.1–3.2's static-viewer stage | The explicit-position path for testing without a real pawn/camera. |
| `ViewerWorldPositionOverride` | `ChunkedPlanet\|Viewer` | `(0,0,0)` | see §3.1's worked example | **UE units (cm)**, relative to the planet's own **center**, not "meters above ground." Easy to get an order-of-magnitude wrong — double-check before concluding something's broken. |

---

## 3. Staged test matrix

Each stage has a specific, falsifiable pass/fail — don't move to the next stage
until the current one actually passes. This is deliberately the same order as §2's
"why" column: geometry before streaming, streaming before scale, scale before real
terrain.

### 3.1 Geometry sanity (static viewer, no terrain, winding material)

**Setup:** `RadiusMeters` = 500, `TerrainStack` unset, `Material` =
`M_SolarOrbz_WindingDebug`, `bUseViewerWorldPositionOverride` = true,
`ViewerWorldPositionOverride` = `(0, 0, 50100)` (≈1 m above a 500 m/50,000 cm-radius
surface, along +Z).

**Pass criteria (all of these):**
- `LogSolarOrbzChunk` lines appear in the Output Log on Play.
- A patch of visibly triangulated sphere surface renders near the override
  position — finer triangles close to it, coarser farther away, not one smooth
  continuous mesh and not the whole sphere (most of a 500 m planet is far from one
  viewer point, so most of it staying coarse/shallow is *correct*).
- **The entire visible surface is green**, per §1.3's material — no red patches
  anywhere you can see from outside, including right at chunk boundaries and skirts.
- `GetResidentChunkCount()` (via `BP_ChunkDebugPawn`'s on-screen print, §1.4) is
  nonzero and stable (not climbing or dropping every frame while the viewer isn't
  moving).

**If it fails:** see §5. Do not proceed to §3.2 with red patches still visible —
every later stage adds more chunks, which only makes a localized winding bug
harder, not easier, to spot.

### 3.2 Streaming reacts to the viewer (still static-then-moved viewer, no terrain)

**Setup:** same as §3.1 to start.

**Test A — approach/retreat:** Move `ViewerWorldPositionOverride` closer to the
surface in a few discrete steps (drag the Z value down toward 50,000, i.e. toward
the actual surface), waiting at least `UpdateIntervalSeconds` after each move (or
press your `ForceChunkUpdate` key, §1.4, for an immediate recompute). **Pass:**
chunk count visibly rises as you approach, and the patch under the viewer gets
finer-grained each step. Retreat and confirm it falls back.

**Test B — boundary dwell (thrashing check):** Find a `ViewerWorldPositionOverride`
value right at a split/merge transition (watch `GetResidentChunkCount()` and nudge
in small steps — your extra key bindings from §1.4 step 4 are exactly for this —
until you find a step where the count flips back and forth as you nudge across it).
Hold the viewer exactly there for 10+ seconds. **Pass:** the chunk settles into one
state within a update or two and stays there — some initial back-and-forth right at
the exact boundary is an acknowledged gap (`FSolarOrbzChunkLODPolicy`'s own header:
hysteresis stops flicker *at a fixed position* but not guaranteed-instant settling
exactly at the dead zone's edge), but it must *settle*, not oscillate forever.
**Fail:** continuous, never-settling popping — that's a real bug in
`ApplyNeighborDepthRestriction`'s fixpoint or the LOD policy itself, not the known
gap, and is worth reporting with the exact position (§6).

**Test C — moving viewer:** Set `bUseViewerWorldPositionOverride` = false,
`ViewerActor` = your possessed `BP_ChunkDebugPawn`. Fly around the planet. **Pass:**
chunks keep resolving around wherever you currently are; a spot you just left visibly
coarsens back out after a few update intervals; a spot you approach visibly refines.

### 3.3 Skirts actually hide something (still no terrain)

With the winding material still assigned, fly close enough to a visible LOD
boundary (a seam between a finer and coarser neighboring chunk — easiest to find
near your §3.2 boundary-dwell spot) and look along the surface, nearly edge-on,
toward that seam. **Pass:** no gap you can see through to empty space/skybox at the
boundary — the near-vertical skirt wall should be hiding it, and (per §1.3) it
should also be green, not red. **Fail:** a visible gap, or a red skirt wall (a
winding bug specific to `AppendSkirts`, not `GenerateChunk` — worth distinguishing
in your report, §6).

### 3.4 Manual-update sanity

Press your `ForceChunkUpdate` key (§1.4) a few times in a row while stationary.
**Pass:** chunk count doesn't change (nothing *should* be different if the viewer
hasn't moved and nothing is mid-transition) and nothing visibly flickers from the
extra calls alone. This is mostly a check that `Update Chunks Now` is wired
correctly and idempotent when nothing's actually changed, not a deep test.

### 3.5 Scaling radius and watching cost

Raise `RadiusMeters` in stages — 10,000 (10 km) → 1,000,000 (1,000 km) → 6,371,000
(Earth) — repeating §3.1's basic sanity check at each stage (still no `TerrainStack`,
still the winding material). At each stage, watch:
- `GetResidentChunkCount()` growth — should grow with radius for a similar
  near-surface viewer distance (more surface area to tile, roughly the same local
  density near the viewer), not explode unboundedly for no reason.
- `stat unit` (console command, §4) at the moment a lot of new chunks spawn at once
  — a frame-time spike there is expected (§0/§2, no frame-budget spreading yet), but
  it should recover immediately afterward, not persist.

**Fail conditions here are performance pathologies, not correctness bugs per se** —
e.g. chunk count growing every single update forever even with a stationary viewer
(a resident-set diffing bug: something that should be "kept, not touched" is being
needlessly regenerated), or a frame-time spike that never recovers (something
leaking components instead of despawning them — check `GetResidentChunkCount()`
isn't silently climbing without bound over several minutes stationary).

### 3.6 Real terrain

Assign a real `TerrainStack` (reuse one already authored for
`ASolarOrbzIcoSphereActor` if one exists in the project). Repeat §3.1's geometry
check and §3.3's skirt check. **New failure mode to specifically look for:** a skirt
that was adequate on a flat, undisplaced sphere no longer fully hiding the seam once
real height variation is involved (a tall cliff right at an LOD boundary can poke
through a skirt sized for a flat crack) — if so, raise `SkirtDepth` substantially
and re-check before concluding it's a deeper bug.

---

## 4. Unreal's own built-in diagnostics worth knowing about

- **Wireframe view mode** (viewport View Mode dropdown, or `Alt+2`) shows actual
  triangle edges directly — the fastest way to *see* chunk boundaries, T-junctions,
  and skirt geometry without needing any custom material. Good complement to §1.3's
  winding material, not a replacement (wireframe doesn't tell you front vs. back).
- **`stat unit`** (console command, backtick/tilde to open console in PIE) — frame
  time breakdown (Game/Draw/GPU), useful for §3.5's performance checks.
- **`stat game`** — more detailed game-thread breakdown if `stat unit` shows the
  game thread as the bottleneck during a chunk-spawn spike.
- **`show collision`** — not useful yet (§0: no collision on streamed chunks), but
  worth knowing it exists for later once collision is actually built.

---

## 5. Troubleshooting

| Symptom | Likely cause | What to check |
|---|---|---|
| Nothing renders at all, no log lines | `BeginPlay` never ran (actor not in a loaded level / not in PIE), or the actor's `RootComponent` is null | Confirm you pressed Play, not just viewing the editor; confirm `BP_ChunkedPlanetTest` actually exists in the level you're playing |
| `LogSolarOrbzChunk` lines appear but nothing visible | Camera is far from the override/viewer position, or the mesh is there but entirely red/culled | Use `BP_ChunkDebugPawn` and fly toward the `ViewerWorldPositionOverride` coordinates directly; confirm `GetResidentChunkCount()` is nonzero first to isolate "nothing generated" from "generated but not visible/reachable" |
| Entire visible surface renders red (§1.3 material) | Triangle winding is backwards — `GenerateChunk`'s own triangulation, not the skirts | This is a real bug, not a known gap — report it (§6) with a screenshot; don't just flip winding by hand without understanding why first, since `GenerateChunk`'s "swap last two corners" fix was explicitly unverified, not definitely wrong — confirm it's consistently backwards (not just some chunks) before concluding the fix needs reversing globally |
| Only skirts render red, the main surface is green | Winding bug specific to `FSolarOrbzChunkSkirtBuilder::AppendSkirts`'s own triangulation | Report separately from a main-surface winding bug (§6) — these are two different functions, likely two different fixes |
| `LogSolarOrbzChunkedPlanet` warning: "no ViewerActor and bUseViewerWorldPositionOverride is false" | Viewer not actually configured despite §2's table | Re-check `bUseViewerWorldPositionOverride` is true AND `ViewerActor` is empty (`ViewerActor` takes priority when both are set) |
| Only ONE giant shallow chunk ever appears, never splits | LOD ratio never crosses `SplitScreenSizeRatio` — usually a units mismatch (meters vs. cm) or a viewer position far from the surface relative to `RadiusMeters` | Double-check `ViewerWorldPositionOverride` is in **cm**, relative to the planet's **center** — §3.1's worked example is `(0,0,50100)`, not `(0,0,1)` |
| Visible cracks/gaps at chunk boundaries despite skirts | `SkirtDepth` too shallow for the actual depth mismatch at that edge (worse with real terrain, §3.6) | Increase `SkirtDepth`; isolate with/without `TerrainStack` (§3.3 vs §3.6) to tell a skirt-sizing issue from a terrain-displacement issue |
| A chunk visibly flickers between split/merged state continuously, never settling | Possible real bug in `ApplyNeighborDepthRestriction`'s fixpoint, or dwelling exactly on a boundary the hysteresis gap doesn't fully cover | §3.2 Test B is built exactly for this — follow it, and if it never settles, report the exact `ViewerWorldPositionOverride`/radius (§6) |
| `GetResidentChunkCount()` keeps climbing forever with a stationary viewer | Resident-set diffing not correctly recognizing "already resident, leave alone" — chunks being regenerated/re-added when they shouldn't be | Confirm the viewer is truly stationary (not drifting slightly); this is a real bug if count keeps climbing with zero viewer movement, worth reporting with exact settings |
| Frame hitches on the tick new chunks appear, otherwise smooth | Expected — `UpdateResidentSet` runs synchronously, no frame-budget spreading yet | Raise `UpdateIntervalSeconds`, or reduce `ChunkResolution`, as a stopgap (§3.5) |
| Editor "Update Chunks Now" button does nothing | Not currently in Play — `ResidentSetManager` only exists between `BeginPlay` and `EndPlay` | Press Play first, then use the button or your `F5` binding (§1.4/§1.5) |
| Fell through the planet | Collision is deliberately not built for streamed chunks yet (item 5's own gap) | Not a bug — `BP_ChunkDebugPawn` as a `SpectatorPawn` has no collision either, by design, to sidestep this entirely during testing |

---

## 6. What to report back

If something looks wrong and isn't resolved by the table above, the fastest way to
get it fixed is to include:

- Which §3 stage you were on, and the exact property values from §2's table at the
  time (especially `RadiusMeters`, `ViewerWorldPositionOverride`/which `ViewerActor`,
  `SkirtDepth`, whether `TerrainStack` was set).
- Whatever `LogSolarOrbzChunk*`/`LogSolarOrbzChunkedPlanet` lines were in the log
  around that time.
- A screenshot if it's visual — **with the §1.3 winding material still assigned if
  the issue might be winding-related at all**, since "red vs. green" pinpoints it
  far faster than a shot with a real material where backfaces just look dark.
- Whether it's the main surface, the skirts, or both that are affected (the
  troubleshooting table above treats these as likely-separate bugs).

That's usually enough to reproduce and fix without a back-and-forth round of
"what were your settings."

---

## 7. What this guide deliberately does not cover

- Tuning `LODSettings`/`SkirtDepth` for a *good-looking* result — both are
  first-pass placeholders per their own header comments, not defaults expected to
  survive contact with a real camera. This guide is about confirming the system
  *works*, not about making it look finished.
- Anything already listed as explicitly out of scope in
  `Docs/ChunkedPlanetTerrain.md` (real vertex-stitching instead of skirts, spawn
  budgeting for a teleport-sized viewer jump, antimeridian handling, baking/Nanite,
  ASN_MK1 gravity/atmosphere integration). None of that is expected to work yet,
  and none of it is this guide's concern.
