# SolarOrbz Terrain Graph Editor — Design

**Status: Phase 1 (data model) implemented. No graph UI exists yet - double-clicking a
`USolarOrbzTerrainLayerStack` asset still opens the generic property editor until Phase 2 lands.**
Written without a UE5.8 compiler available (see every other SolarOrbz doc's standing caveat) -
`UAssetDefinition`/`GetMipImage`/`UEdGraph`-class facts below were checked against current
documentation before being written as fact, per this repo's own UE5.8-specific documentation
standard (`CLAUDE.md`); Phase 1's code (`SolarOrbzTerrainGraph.h`/`.cpp`) is therefore at this
repo's "review-only" verification bar - checked by careful reading against those documented APIs,
not compiled or run, same as `AASolarOrbzChunkedPlanetActor`/`FSolarOrbzChunkResidentSetManager` in
the chunked-terrain work.

## Why this exists

Requested directly: a dedicated UI for authoring a planet's terrain recipe, and specifically a node
graph - add nodes for each terrain contribution, wire them together, rather than editing an
`Instanced` object array in the generic Details panel. Modeled on World Creator's filter-stack idea
(already the runtime model - see `Docs/ChunkedPlanetTerrain.md`'s sibling systems and `ROADMAP.md`'s
Terrain section), but surfaced as an actual visual graph instead of an array widget.

## The one decision that keeps this low-risk: the graph is a VIEW, not a new data format

`USolarOrbzTerrainLayerStack::Layers` (an ordered `TArray<TObjectPtr<USolarOrbzTerrainLayer>>`) is
**not going away and is not being duplicated into a second, parallel source of truth.** It stays
the only thing `EvaluateHeight`/`EvaluateHeightUpTo`/`Bake`/`ApplyPlanetaryContext`/every existing
caller ever reads - none of that code changes. The graph is a transient (`Transient` UPROPERTY,
never saved itself) `UEdGraph` **rebuilt fresh from `Layers` every time the asset editor opens**, and
every structural edit in the graph (add node, remove node, reorder/reconnect) immediately writes
straight back into `Layers` in the new order. This is deliberately the same posture the chunked
terrain work took toward engine-dependent code: keep already-working, already-tested runtime logic
completely untouched, and build the new surface as a layer strictly on top of it.

Two small, genuinely new pieces of persisted data make this possible:

- **`FGuid EditorNodeId` on the base `USolarOrbzTerrainLayer` class** (generated once,
  `FGuid::NewGuid()`, the first time a layer instance needs one) - a stable identity for "which saved
  node position belongs to which layer" that survives reordering/inserting/removing layers, which a
  plain array index would not.
- **`TMap<FGuid, FVector2D> EditorNodePositions` on `USolarOrbzTerrainLayerStack`** - editor-only
  layout memory (so your node arrangement doesn't reset to a default grid every time you reopen the
  asset). Everything else about the graph (which nodes exist, their order, their properties) is
  already fully determined by `Layers` itself and needs no separate storage.

## Why a strict chain, not an arbitrary node graph, for v1

The actual evaluation model (`USolarOrbzTerrainLayerStack::EvaluateHeight`) is an ordered accumulator
- each layer reads "everything below it" and blends in (Add/Multiply/Min/Max/Subtract/Replace,
first enabled layer always an implicit Replace) via its own `Strength`/`Mask`. That is inherently a
straight line, not a tree - there is no existing semantic for "combine two independent branches" to
expose through a richer graph shape yet. So v1's schema enforces exactly what the array already
allows: one Height-In pin, one Height-Out pin, per node, each connectable to exactly one neighbor -
a visual, reorderable chain, not a new evaluation model. A fixed `Start` node (no input - the base
sphere before any layer) and a fixed `Output` node (no output - what the actor actually samples)
bookend the chain. Branching/merge nodes are explicitly **out of scope for v1** (see below) - the
runtime would need real new semantics for "how do two branches combine" before a graph UI could
expose it meaningfully, and that's a bigger, separate design question, not a graph-editor question.

## Architecture, phased

### Phase 1 - data model (implemented, `SolarOrbzTerrainGraph.h`/`.cpp` + additions to
`SolarOrbzTerrainLayers.h`/`.cpp`)

- `FGuid USolarOrbzTerrainLayer::EditorNodeId` (new field on the existing base class) +
  `EnsureEditorNodeId()` (lazily assigns a new Guid if not already valid) - does not affect
  `GetRawHeight`/`Bake`/anything evaluation-related.
- `TMap<FGuid, FVector2D> USolarOrbzTerrainLayerStack::EditorNodePositions` (new field, editor-only
  layout memory) and `TObjectPtr<USolarOrbzTerrainGraph> USolarOrbzTerrainLayerStack::TerrainGraph`
  (new field, `Transient, DuplicateTransient`) + `GetOrCreateTerrainGraph()` (builds/returns it,
  calling `RebuildFromLayers()` on first access).
- `USolarOrbzTerrainGraphNode : UEdGraphNode` - ONE generic node class, not one subclass per layer
  type. It owns a direct reference to the real layer instance from `Layers` (`UPROPERTY()
  TObjectPtr<USolarOrbzTerrainLayer> Layer` - the SAME object, not a copy, so editing the node's
  properties via the Details panel edits the real asset data with no sync step needed), exposes one
  Height-In and one Height-Out pin (except the fixed Start/Output sentinel nodes), and its title/
  tooltip are derived from the wrapped layer's class (`UClass::GetDisplayNameText()`, which reads
  the class's own `DisplayName` meta, e.g. "Noise Layer", "Heightmap Layer" - already defined on
  every concrete layer class today). This is also why adding a brand new `USolarOrbzTerrainLayer`
  subclass later needs ZERO new graph-side code - the "Add Node" menu (Phase 2) enumerates layer
  subclasses via reflection, not a hardcoded list.
- `USolarOrbzTerrainGraphSchema : UEdGraphSchema` - overrides `CanCreateConnection` to enforce the
  chain shape: a Height-In/Height-Out pin connects to exactly one neighbor, and dragging a new wire
  onto a pin that already has one replaces it (`CONNECT_RESPONSE_BREAK_OTHERS_A/B/AB`) rather than
  allowing a second - the same posture Blueprint's own K2 schema takes with input pins.
- `USolarOrbzTerrainGraph : UEdGraph` - owned transiently by the stack asset (see
  `TerrainGraph`/`GetOrCreateTerrainGraph()` above), built on first access, never serialized. Two
  directions:
  - `RebuildFromLayers()` - discards whatever nodes the graph currently has, then walks `Layers` in
    order, creates one `USolarOrbzTerrainGraphNode` per entry (reusing `EditorNodePositions` for
    layout, defaulting to an evenly-spaced row for any layer with no saved position yet - e.g. a
    stack authored before this existed), wires them in a chain between the fixed Start/Output nodes.
  - `CompileToLayers()` - walks the chain from Start to Output, writes the visited nodes' `Layer`
    references into `Layers` in that order (this is what actually "applies" a reorder/add/remove
    made in the graph back onto the real asset data) and snapshots each visited node's current
    canvas position into `EditorNodePositions`.

Nothing calls `GetOrCreateTerrainGraph()` yet - without Phase 2's editor toolkit, this is dead code
reachable only from C++/the future editor, not from anything a user can trigger today. It compiles
against documented UE5.8 `UEdGraph`/`UEdGraphNode`/`UEdGraphSchema` APIs (Engine module, already a
`SolarOrbz.Build.cs` dependency - no Build.cs change was needed for this phase) but has not been run
in-editor, per the review-only bar noted above.

### Phase 2 - the actual editor UI (not started)

- `USolarOrbzTerrainLayerStackAssetDefinition : UAssetDefinitionDefault` - the modern UE5.8
  registration point (Epic's current replacement for the older `IAssetTypeActions`/
  `FAssetTypeActions_Base` pattern - `UAssetDefinition`s register automatically with
  `UAssetDefinitionRegistry` and route a double-click through the new `UToolMenu`-based actions
  system rather than a hand-registered `IAssetTypeActions::OpenAssetEditor` override). This is what
  makes double-clicking a `USolarOrbzTerrainLayerStack` asset open the new editor instead of the
  generic property matrix.
- `FSolarOrbzTerrainGraphEditorToolkit : FAssetEditorToolkit` - the actual editor window/tab layout:
  an `SGraphEditor` (from the `GraphEditor` module) bound to `RebuildFromLayers()`'s output, docked
  alongside a standard `IDetailsView` showing whichever node is currently selected - this is also
  why Phase 1's "node just references the real layer object" choice matters: the existing Details
  panel already knows how to edit every `UPROPERTY` on every existing layer type (`Strength`,
  `BlendMode`, `Mask`, every `Noise`/`Heightmap`/`Erosion`-specific field) with zero new UI code -
  selecting a node is all that's needed to get the exact same property editing experience the array
  widget already provides today, just reached by clicking a node instead of expanding an array
  entry.
- "Add Node" context menu, reflection-driven over non-abstract `USolarOrbzTerrainLayer` subclasses.
- Wire `CompileToLayers()` to fire on every structural graph edit (`FEdGraphEditAction`), not just on
  save, so `Layers` never drifts out of sync with what the graph currently shows.

### Phase 3 - explicitly future, not v1

- Branching/merge nodes, once there's an actual runtime semantic for combining independent chains
  (a real evaluation-model question, not just a graph-UI one).
- Per-node live preview thumbnails.
- Categorized/grouped Add Node menu, comment boxes, copy/paste between stacks.

## Known limitations / out of scope for this pass

- No graph UI yet - Phase 1 (data model) only. A stack asset still opens the generic editor until
  Phase 2 lands, and nothing calls `GetOrCreateTerrainGraph()` today.
- `UEdGraph`/`UEdGraphNode`/`SGraphEditor`-class code is engine-dependent UObject/Slate machinery,
  the same "review-only" verification bar as `AASolarOrbzChunkedPlanetActor`/
  `FSolarOrbzChunkResidentSetManager` in the chunked-terrain work - there is no Python-equivalent
  ground truth for "does this UEdGraphSchema correctly reject an invalid Slate drag-connection," so
  this is checked by careful reading against documented UE5.8 APIs and this plugin's own existing
  patterns, not compiled or run (no UE5.8 compiler available in this environment).
- Branching/merge terrain combination - genuinely not designed yet, see Phase 3.
