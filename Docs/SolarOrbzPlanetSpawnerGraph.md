# SolarOrbz Planet Spawner Graph — Design

**Status: implemented, not yet exercised hands-on.** Written without a UE5.8 compiler available
(see every other SolarOrbz doc's standing caveat) - `SGraphEditor`/`IDetailsView`/
`TStrongObjectPtr`-class facts below were checked against current documentation before being
written as fact, per this repo's own UE5.8-specific documentation standard (`CLAUDE.md`); this code
is therefore at this repo's "review-only" verification bar - checked by careful reading against
those documented APIs, not compiled or run.

## Why this exists

Requested directly: turn the SolarOrbz dock tab's flat "enter Radius, enter Vertices Per Meter,
click Generate" panel into a node graph, the same visual system as the Terrain Graph Editor
(`Docs/SolarOrbzTerrainGraphEditor.md`).

A first pass added a CSV/DataTable "Planet Catalog" to fill the graph's modules from a prebuilt
row. That was then explicitly thrown out in favor of a different idea: rather than driving a
planet's modules from spreadsheet rows, embed the real terrain-layer authoring chain - the same
`Start -> Layer -> ... -> Output` mechanism the standalone Terrain Graph Editor uses, noise layers
included - directly into this graph's own canvas, so a terrain recipe is authored node-by-node in
the same place the rest of the planet is assembled, with no separate `USolarOrbzTerrainLayerStack`
asset required. Biome Stack deliberately stays a plain external asset reference - only Terrain gets
inlined, because only Terrain has the "ordered chain of layers" shape that makes inlining pay off;
Biome/Climate/Profile are each a single assignment, not a chain.

## Two different topologies in one graph

The fixed modules (Base Sphere, Biome Stack, Climate Simulation, Profile) have no inherent order -
four independent assignments, not steps in a blend. Forcing a fake chain onto them would teach the
wrong mental model, so they stay a **fixed, non-editable layout**: one node per module, each wired
to a single "Planet" sentinel purely to show them as belonging to one planet.
`USolarOrbzPlanetSpawnerGraphSchema::CanCreateConnection` disallows any new connection between pins
using `USolarOrbzPlanetSpawnerGraphNode::ModulePinCategory` - there is no "Add Node" and no
reconnecting for this part of the graph, ever.

Terrain is the opposite: a real ordered chain, same semantics as the standalone Terrain Graph
Editor, laid out on its own row of the same canvas and fully user-editable (add a layer, reconnect,
delete) via `USolarOrbzTerrainGraphNode` - reused directly rather than reinvented, since that class
already has zero graph-type-specific logic. The schema allows normal single-connection/replace-on-
reconnect behavior for these pins (`USolarOrbzTerrainGraphNode::HeightPinCategory`), same as the
standalone editor's own schema.

```
 Base Sphere ─┐
 Biome Stack  ─┼──► Planet         (fixed, not user-editable)
 Climate Sim  ─┤
 Profile      ─┘

 Start ──► [Layer] ──► [Layer] ──► ... ──► Output     (embedded terrain chain, user-editable)
```

Both halves share one `SGraphEditor`/`IDetailsView` split and one "select a node, edit its real
object via Details, zero sync step" pattern - the fixed modules show a `ModuleConfig` object,
terrain-chain nodes show the real `USolarOrbzTerrainLayer` they wrap, exactly like the standalone
editor.

## Architecture

### Module config objects (`SolarOrbzPlanetSpawnerGraph.h`/`.cpp`)

Four small, transient `UObject`s, one per fixed module, each holding exactly the properties that
module needs - nothing else, so selecting a node shows exactly one concern in the Details panel:

- `USolarOrbzPlanetBaseSphereConfig` - `RadiusMeters`/`VerticesPerMeter`/`MaxSubdivisions`/
  `bEnablePreviewCollision` (the same fields the original flat panel staged).
- `USolarOrbzPlanetBiomeModule` - `BiomeStack` (`TObjectPtr<USolarOrbzBiomeStack>`).
- `USolarOrbzPlanetClimateModule` - `ClimateSimulation` (`TObjectPtr<USolarOrbzClimateSimulationAsset>`).
- `USolarOrbzPlanetProfileModule` - `Profile` (`TObjectPtr<USolarOrbzCelestialBodyProfile>`).

None of these are assets - they're pure in-memory editor scratch state, never saved, existing only
for as long as the SolarOrbz dock tab is open. Terrain has no such config object anymore - it's
represented by the embedded chain below instead.

### Graph classes

- `USolarOrbzPlanetSpawnerGraphNode : UEdGraphNode` - ONE generic node class for the fixed side
  (same "wraps a real object directly" pattern as the Terrain Graph Editor's node), with a
  `ModuleConfig` reference and a `DisplayTitle` set directly by `BuildFixedLayout()`. The fixed
  "Planet" sentinel (`bIsPlanetNode = true`) has four named input pins ("Base Sphere"/"Biome"/
  "Climate"/"Profile"); every other fixed-side node has one output pin ("Out").
- `USolarOrbzTerrainGraphNode : UEdGraphNode` (from `SolarOrbzTerrainGraph.h`, unmodified) - reused
  as-is for the embedded terrain chain's Start/Output sentinels and per-layer nodes. It's
  graph-agnostic (never references its owning graph's type), so nothing about it needed changing to
  live in a second graph class.
- `USolarOrbzPlanetSpawnerGraphSchema : UEdGraphSchema` - `CanCreateConnection` checks
  `PinType.PinCategory` first: either side being `ModulePinCategory` disallows outright (fixed
  topology); otherwise it's a terrain-chain `HeightPinCategory` pin, and the same
  single-connection/replace-on-reconnect logic `USolarOrbzTerrainGraphSchema` already proved out
  applies (duplicated into this schema rather than inherited, since this schema alone needs the
  module-category branch first).
- `USolarOrbzPlanetSpawnerGraph : UEdGraph` - `BuildFixedLayout()` creates the four fixed config
  objects and five fixed nodes (four modules + Planet), wires them once, creates a transient
  `EmbeddedTerrainStack`, and calls `RebuildEmbeddedTerrainChain()` to seed the (initially empty)
  terrain chain alongside them in the same `Nodes` array. `RebuildEmbeddedTerrainChain()`/
  `CompileEmbeddedTerrainChain()` duplicate `USolarOrbzTerrainGraph::RebuildFromLayers()`/
  `CompileToLayers()`'s algorithms exactly, with one necessary difference: they filter `Nodes` by
  `Cast<USolarOrbzTerrainGraphNode>` before touching anything, so rebuilding the terrain chain never
  disturbs the fixed module/Planet nodes sharing that same array - the original methods' "discard
  every node" approach is only safe because `USolarOrbzTerrainGraph` owns its entire graph alone.
  `bIsRebuildingTerrainChain`/`IsRebuildingTerrainChain()` guards against the panel's graph-changed
  handler compiling a half-rebuilt chain, mirroring `USolarOrbzTerrainGraph::IsRebuilding()`.

Exposes `BaseSphereConfig`/`BiomeModule`/`ClimateModule`/`ProfileModule`/`EmbeddedTerrainStack`
directly, so the panel's Generate logic reads them by name rather than walking `Nodes` for the
fixed side - deleting a fixed module node (Delete key) only removes that module's Details-panel
entry point, it doesn't lose the underlying config or break Generate; Ctrl+Z restores it. The
terrain chain is different: that part of `Nodes` **is** the source of truth for
`EmbeddedTerrainStack->Layers`, exactly as it is for an external stack in the standalone editor.

### The panel (`SolarOrbzEditor.h`/`.cpp`, `SSolarOrbzMainPanel`)

The dock tab itself didn't change identity - same `Window → SolarOrbz` entry, same toolbar button,
same tab. Its content:

- An `SGraphEditor` bound to a `USolarOrbzPlanetSpawnerGraph` (built once in `Construct()`), split
  alongside an `IDetailsView` that follows node selection (`HandleGraphSelectionChanged`, extended to
  recognize both `USolarOrbzPlanetSpawnerGraphNode::ModuleConfig` and
  `USolarOrbzTerrainGraphNode::Layer`) - directly mirrors the Terrain Graph Editor's toolkit, just
  without the `FAssetEditorToolkit`/tab-manager machinery, since this isn't an asset editor opened
  by double-click; it's the same single Nomad dock tab the plugin always had.
- `SpawnerGraph` is held as a `TStrongObjectPtr`, not a `TObjectPtr`/`UPROPERTY` - `SSolarOrbzMainPanel`
  is a Slate widget, not a `UObject`, so there's no reflected property to anchor the graph's GC root
  the way `USolarOrbzTerrainLayerStack::TerrainGraph` anchors the Terrain Graph Editor's graph. A
  `TStrongObjectPtr` is the standard tool for exactly this - "a plain C++ object needs to keep a
  `UObject` alive" - and releases automatically when the panel is destroyed (dock tab closed).
- An **Add Layer** toolbar combo button above the graph canvas, reflection-driven over non-abstract
  `USolarOrbzTerrainLayer` subclasses (`BuildAddLayerMenu()`/`AddLayerOfClass()`) - the exact same
  pattern as the standalone Terrain Graph Editor's toolkit, so a brand-new layer type needs zero new
  code in either place.
- `HandleSpawnerGraphChanged`, bound via `SpawnerGraph->AddOnGraphChangedHandler(...)` in
  `Construct()`, calls `CompileEmbeddedTerrainChain()` on every genuine interactive edit to the
  terrain chain (guarded by `IsRebuildingTerrainChain()`) - mirrors
  `FSolarOrbzTerrainGraphEditorToolkit::HandleGraphChanged` exactly. No explicit destructor removes
  this handler (unlike the toolkit's `RemoveOnGraphChangedHandler` in its own destructor) - the panel
  and its `SpawnerGraph` share one lifetime, and the delegate's `CreateSP` binding already no-ops
  safely if the panel outlives the binding somehow.
- `OnGenerateClicked` calls `SpawnerGraph->CompileEmbeddedTerrainChain()` defensively before reading
  `EmbeddedTerrainStack`, then assigns it straight to `Actor->TerrainStack` - the same field a
  regular external `USolarOrbzTerrainLayerStack` reference would have gone into, since
  `ASolarOrbzIcoSphereActor` doesn't know or care whether the stack it's pointed at is a saved asset
  or a dock tab's transient scratch object.

### Chunked Planet Preview section

A second preview path, added once planet-scale radii became the actual target: `ASolarOrbzIcoSphereActor`
is a single mesh, so a real planetary radius (e.g. Earth's 6,371,000 m) only ever produces one coarse
polygon ball - `Docs/ChunkedPlanetTerrain.md`'s own `AASolarOrbzChunkedPlanetActor` exists specifically
for ground-level detail at that scale, but had no way to see it without entering Play. `OnGenerateChunkedClicked`
spawns/rebuilds one, reusing exactly the same `BaseSphereConfig->RadiusMeters` and `EmbeddedTerrainStack`
(plus Biome/Climate/Profile, forwarded for parity even though the chunked actor doesn't read the last
three yet) the simple preview above uses - same noise functions, same terrain chain, just a different
representation of the result, so "does this recipe look good enough" can be checked at real scale
without hand-configuring a second actor.

Two gaps in `AASolarOrbzChunkedPlanetActor` itself had to be fixed first, independent of this panel:
- `UpdateChunksNow()` was `CallInEditor` but silently did nothing on an actor that had never been
  through `BeginPlay()` (i.e. always, outside PIE/a packaged game) - its `ResidentSetManager` was
  only ever constructed there. Fixed by factoring that construction into `ConstructResidentSetManager()`,
  called lazily by `UpdateChunksNow()` the first time.
- Changing `RadiusMeters`/`TerrainStack`/etc. on an already-constructed manager did nothing - it has
  no setters, so it keeps using whatever it was first built with. Fixed by a new
  `RebuildChunkedPlanetNow()` (tears the manager down, reconstructs it from current properties, then
  recomputes) - the chunked actor's equivalent of `RegenerateMesh()`. This panel's button always
  calls this one, not `UpdateChunksNow()`, matching the simple preview's "every Generate click
  rebuilds from current values" semantics.

There's no Pawn/camera to resolve a real viewer position from outside Play, so the button defaults
the spawned actor's `ViewerWorldPositionOverride` to a fixed altitude (2 m) above the north pole on
every click - unless a real `ViewerActor` is assigned, which always wins and is never overwritten
here. This streams chunks near that ONE vantage point, not the whole globe at max depth everywhere -
inherent to why chunked streaming exists at all, not a gap in this integration. To look elsewhere:
edit the spawned actor's own `ViewerWorldPositionOverride` directly in its Details panel and press
ITS OWN `RebuildChunkedPlanetNow` button - that bypasses this panel's click handler, so it won't get
reset back to the north pole until the panel's own button is pressed again.

## Known limitations / out of scope for this pass

- `SGraphEditor`/`IDetailsView`/`TStrongObjectPtr`-class code is engine-dependent UObject/Slate
  machinery, the same review-only verification bar as the Terrain Graph Editor and the
  chunked-terrain work - checked by careful reading against documented UE5.8 APIs, not compiled or
  run.
- `EmbeddedTerrainStack` is never saved as its own asset - if a user wants to reuse one planet's
  terrain recipe on another, there's no "Save As" from this panel; the only way to reuse a recipe
  across planets today is the standalone Terrain Graph Editor's real asset workflow.
- No batch/multi-planet spawning - this pass is one panel driving one preview actor at a time, same
  as before.
- Deleting a fixed module node (Delete key) hides that module's Details-panel entry point until
  Ctrl+Z - there's no "restore node" affordance beyond undo. Deleting a terrain-chain node behaves
  like the standalone editor: the chain truncates until reconnected (or undone).
- The Chunked Planet Preview only ever shows ground-level detail near ONE vantage point per click -
  not the whole planet at max depth (chunked streaming exists specifically to avoid ever doing
  that). No biome coloring/climate masking either - `AASolarOrbzChunkedPlanetActor` forwards
  Biome/Climate/Profile for parity but doesn't read them yet (see its own header comment), so a
  streamed chunk shows raw terrain shape only, same gap that exists outside this panel.
