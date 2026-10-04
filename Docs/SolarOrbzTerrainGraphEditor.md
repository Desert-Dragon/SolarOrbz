# SolarOrbz Terrain Graph Editor — Design

**Status: Phase 1 (data model) and Phase 2 (editor UI) both implemented.** Double-clicking a
`USolarOrbzTerrainLayerStack` asset now opens `FSolarOrbzTerrainGraphEditorToolkit` instead of the
generic property matrix. Written without a UE5.8 compiler available (see every other SolarOrbz
doc's standing caveat) - `UAssetDefinition`/`GetMipImage`/`UEdGraph`/`SGraphEditor`/
`FAssetEditorToolkit`-class facts below were checked against current documentation before being
written as fact, per this repo's own UE5.8-specific documentation standard (`CLAUDE.md`); this
code (`SolarOrbzTerrainGraph.h`/`.cpp`, `SolarOrbzTerrainGraphEditorToolkit.h`/`.cpp`) is therefore
at this repo's "review-only" verification bar - checked by careful reading against those documented
APIs, not compiled or run, same as `AASolarOrbzChunkedPlanetActor`/`FSolarOrbzChunkResidentSetManager`
in the chunked-terrain work. Every operation in both files now logs through `LogSolarOrbzTerrainGraph`
(see "Logging" below) specifically so a real compile/run pass has something to point at if any of
this guessed an API shape wrong.

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

`GetOrCreateTerrainGraph()` is now called from Phase 2's toolkit on editor open - see below. It
compiles against documented UE5.8 `UEdGraph`/`UEdGraphNode`/`UEdGraphSchema` APIs (Engine module,
already a `SolarOrbz.Build.cs` dependency - no Build.cs change was needed for this phase) but has
not been run in-editor, per the review-only bar noted above.

### Phase 2 - the actual editor UI (implemented, `SolarOrbzTerrainGraphEditorToolkit.h`/`.cpp`)

- `USolarOrbzTerrainLayerStackAssetDefinition : UAssetDefinitionDefault` - the modern UE5.8
  registration point (Epic's current replacement for the older `IAssetTypeActions`/
  `FAssetTypeActions_Base` pattern - `UAssetDefinition`s register automatically with
  `UAssetDefinitionRegistry`, no manual registration call needed anywhere in this module). Overrides
  `GetAssetClass`/`GetAssetDisplayName`/`GetAssetColor`/`GetAssetCategories` (puts the asset under a
  "SolarOrbz" category) and `OpenAssets`, which is what makes double-clicking a
  `USolarOrbzTerrainLayerStack` asset open `FSolarOrbzTerrainGraphEditorToolkit` below instead of the
  generic property matrix. Lives in the `UnrealEd` module (already a dependency); the `FAssetOpenArgs`/
  `FAssetCategoryPath`/`EAssetCommandResult` types it uses live in the `AssetDefinition` module,
  newly added to `SolarOrbz.Build.cs`.
- `FSolarOrbzTerrainGraphEditorToolkit : FAssetEditorToolkit` - the actual editor window/tab layout:
  an `SGraphEditor` (from the newly-added `GraphEditor` module dependency) bound to the stack's
  `USolarOrbzTerrainGraph`, in a "Terrain Graph" tab alongside a "Details" tab holding a standard
  `IDetailsView`. `InitEditor()` calls `GetOrCreateTerrainGraph()` then `RebuildFromLayers()` again
  unconditionally (covers a stack whose `Layers` changed via the generic Details panel - e.g.
  undo/redo - while this graph wasn't open to see it), then builds both widgets and lays them out via
  `FTabManager::NewLayout` (75/25 horizontal split) before calling `InitAssetEditor()`.
  - Selecting a node pushes its wrapped `Layer` object into the `IDetailsView` (`HandleSelectionChanged`,
    bound to `SGraphEditor::FGraphEditorEvents::OnSelectionChanged`) - this is also why Phase 1's
    "node just references the real layer object" choice matters: the existing Details panel already
    knows how to edit every `UPROPERTY` on every existing layer type (`Strength`, `BlendMode`, `Mask`,
    every `Noise`/`Heightmap`/`Erosion`-specific field) with zero new UI code.
  - **"Add Layer" is a toolbar combo button, not a native graph right-click context menu - a
    deliberate simplification from the original design.** `SGraphEditor`'s right-click "create node"
    path goes through an `OnCreateActionMenu` delegate whose exact signature could not be pinned down
    against documented UE5.8 sources with the confidence this project's verification standard calls
    for. A combo button (`BuildAddLayerMenu`, reflection-driven over non-abstract
    `USolarOrbzTerrainLayer` subclasses via `GetDerivedClasses` - still zero per-layer-type code, same
    promise the original design made) reaches the same outcome through APIs already proven correct
    elsewhere in this codebase (`SComboButton`/`SButton`, the same pattern `SolarOrbzEditor.h`'s
    `SSolarOrbzMainPanel` already uses). Picking a class calls `AddLayerOfClass()`, which appends a
    `NewObject` of that class to `Layers` and calls `RebuildFromLayers()` to show it immediately.
  - `CompileToLayers()` fires on every structural graph edit via `TerrainGraph->AddOnGraphChangedHandler`
    (`HandleGraphChanged`), not just on save, so `Layers` never drifts out of sync with what the graph
    currently shows - exactly the Phase 2 item the original design called for. This needed one small
    Phase 1 addition made alongside this phase: `USolarOrbzTerrainGraph::IsRebuilding()` (backed by a
    new private `bIsRebuilding` flag, set for the duration of `RebuildFromLayers()`). Without it,
    `RebuildFromLayers()`'s own node adds/removes - which broadcast the identical graph-changed
    notification a user's interactive edit does - would reach `HandleGraphChanged` mid-rebuild (e.g.
    after Start and Output both exist but before any layer node is wired between them) and compile a
    truncated or empty chain back into `Layers`. `HandleGraphChanged` checks `IsRebuilding()` before
    calling `CompileToLayers()`; genuine interactive edits (the whole point) aren't affected.

### Phase 3 - explicitly future, not v1

- Branching/merge nodes, once there's an actual runtime semantic for combining independent chains
  (a real evaluation-model question, not just a graph-UI one).
- Per-node live preview thumbnails.
- Categorized/grouped Add Node menu, comment boxes, copy/paste between stacks.

## Logging

Every operation in both files logs through one shared category, `LogSolarOrbzTerrainGraph`
(declared in `SolarOrbzTerrainGraph.h`, defined in `SolarOrbzTerrainGraph.cpp`) - filtering the
Output Log for it shows the whole story for one editor session: `RebuildFromLayers`/
`CompileToLayers` (start/finish, layer counts, a null-entry or broken-chain warning),
`CanCreateConnection`'s allow/disallow/replace decisions (`Verbose`), `GetOrCreateTerrainGraph`'s
first-build, and the toolkit's open/close, `AddLayerOfClass`, `HandleSelectionChanged`, and
`HandleGraphChanged`. This exists specifically so that if any of this guessed a UE5.8 API shape
wrong, the first real compile/run pass has a trail to debug from rather than a silent failure -
e.g. a `CompileToLayers` "chain never reached Output" warning would point straight at a
`CanCreateConnection` or `RebuildFromLayers` bug rather than silently truncating `Layers`.

## Known limitations / out of scope for this pass

- `UEdGraph`/`UEdGraphNode`/`SGraphEditor`/`FAssetEditorToolkit`-class code is engine-dependent
  UObject/Slate machinery, the same "review-only" verification bar as
  `AASolarOrbzChunkedPlanetActor`/`FSolarOrbzChunkResidentSetManager` in the chunked-terrain work -
  there is no Python-equivalent ground truth for "does this UEdGraphSchema correctly reject an
  invalid Slate drag-connection" or "does this toolkit's tab layout actually render," so this is
  checked by careful reading against documented UE5.8 APIs and this plugin's own existing patterns,
  not compiled or run (no UE5.8 compiler available in this environment). The logging above exists
  specifically to make the first real compile/run pass debuggable rather than starting over blind.
- "Add Layer" is a toolbar combo button, not a native graph right-click context menu - see Phase 2
  above for why. A right-click "Add Node" experience is still achievable later, once
  `OnCreateActionMenu`'s signature can be confirmed against a real engine checkout or documentation
  that pins it down precisely.
- Branching/merge terrain combination - genuinely not designed yet, see Phase 3.
