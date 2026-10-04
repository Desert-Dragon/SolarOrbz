# SolarOrbz Planet Spawner Graph — Design

**Status: implemented, not yet exercised hands-on.** Written without a UE5.8 compiler available
(see every other SolarOrbz doc's standing caveat) - `SObjectPropertyEntryBox`/`UDataTable`/
`SGraphEditor`/`TStrongObjectPtr`-class facts below were checked against current documentation
before being written as fact, per this repo's own UE5.8-specific documentation standard
(`CLAUDE.md`); this code is therefore at this repo's "review-only" verification bar - checked by
careful reading against those documented APIs, not compiled or run.

## Why this exists

Requested directly: turn the SolarOrbz dock tab's flat "enter Radius, enter Vertices Per Meter,
click Generate" panel into a node graph, the same visual system as the Terrain Graph Editor
(`Docs/SolarOrbzTerrainGraphEditor.md`) - and separately, let a single CSV row fill in a planet's
base information (radius, biome stack, etc.) instead of authoring every field by hand, since these
are meant to be relatively procedural planets, built with as little manual entry as possible.

## Why this graph looks different from the Terrain Graph Editor's

The Terrain Graph Editor's chain has real meaning: `Layers` is an ordered accumulator, so left-to-
right position in that graph **is** evaluation order. A planet's base information has no such
order - Radius, Terrain Stack, Biome Stack, Climate Simulation, and Profile are five independent
assignments, not steps in a blend. Forcing a fake chain onto them (as if "Terrain Stack" ran before
"Biome Stack") would teach the wrong mental model. So this graph is a **fixed, non-editable
layout**: one node per module, each wired to a single "Planet" sentinel purely to show them as
belonging to one planet - not a chain you reorder. The only interactions are dragging nodes around
the canvas and selecting one to edit its module's fields in the Details panel. There is no "Add
Node" and no reconnecting: `USolarOrbzPlanetSpawnerGraphSchema::CanCreateConnection` always
disallows, and the five module nodes plus the Planet sentinel are the whole graph, always.

```
 Base Sphere  ─┐
 Terrain Stack ─┤
 Biome Stack   ─┼──► Planet
 Climate Sim   ─┤
 Profile       ─┘
```

This is still "a graph similar to the Terrain Graph Editor" in every way that matters for the
request: same `SGraphEditor`/`IDetailsView` split, same "select a node, edit its real object via
Details, zero sync step" pattern, same visual language - it just doesn't pretend the planet's
modules have an order they don't have.

## Architecture

### Module config objects (`SolarOrbzPlanetSpawnerGraph.h`/`.cpp`)

Five small, transient `UObject`s, one per module, each holding exactly the properties that module
needs - nothing else, so selecting a node shows exactly one concern in the Details panel:

- `USolarOrbzPlanetBaseSphereConfig` - `RadiusMeters`/`VerticesPerMeter`/`MaxSubdivisions`/
  `bEnablePreviewCollision` (the same fields the old flat panel staged).
- `USolarOrbzPlanetTerrainModule` - `TerrainStack` (`TObjectPtr<USolarOrbzTerrainLayerStack>`).
- `USolarOrbzPlanetBiomeModule` - `BiomeStack` (`TObjectPtr<USolarOrbzBiomeStack>`).
- `USolarOrbzPlanetClimateModule` - `ClimateSimulation` (`TObjectPtr<USolarOrbzClimateSimulationAsset>`).
- `USolarOrbzPlanetProfileModule` - `Profile` (`TObjectPtr<USolarOrbzCelestialBodyProfile>`).

None of these are assets - they're pure in-memory editor scratch state, never saved, existing only
for as long as the SolarOrbz dock tab is open.

### Graph classes

- `USolarOrbzPlanetSpawnerGraphNode : UEdGraphNode` - ONE generic node class (same "wraps a real
  object directly" pattern as the Terrain Graph Editor's node), with a `ModuleConfig` reference and
  a `DisplayTitle` set directly by `BuildFixedLayout()` (there's no polymorphic "layer type" to
  derive a title from here - every node's shape is already known, unlike Terrain's reflection-driven
  layer types). The fixed "Planet" sentinel (`bIsPlanetNode = true`) has five named input pins
  ("Base Sphere"/"Terrain"/"Biome"/"Climate"/"Profile"); every other node has one output pin
  ("Out").
- `USolarOrbzPlanetSpawnerGraphSchema : UEdGraphSchema` - `CanCreateConnection` always disallows.
  The topology is built once and never changes.
- `USolarOrbzPlanetSpawnerGraph : UEdGraph` - `BuildFixedLayout()` creates the five config objects
  and six nodes (five modules + Planet) and wires them, once. Exposes
  `BaseSphereConfig`/`TerrainModule`/`BiomeModule`/`ClimateModule`/`ProfileModule` directly, so the
  panel's Generate/Apply Row logic reads them by name rather than walking `Nodes` - which also means
  deleting a node in the graph (Delete key) only removes that module's Details-panel entry point, it
  doesn't lose the underlying config or break Generate; Ctrl+Z (the graph's nodes are
  `RF_Transactional`) restores it.

### The panel (`SolarOrbzEditor.h`/`.cpp`, `SSolarOrbzMainPanel`)

The dock tab itself didn't change identity - same `Window → SolarOrbz` entry, same toolbar button,
same tab. Its content did:

- An `SGraphEditor` bound to a `USolarOrbzPlanetSpawnerGraph` (built once in `Construct()`), split
  alongside an `IDetailsView` that follows node selection (`HandleGraphSelectionChanged`) - directly
  mirrors the Terrain Graph Editor's toolkit, just without the `FAssetEditorToolkit`/tab-manager
  machinery, since this isn't an asset editor opened by double-click; it's the same single Nomad
  dock tab the plugin always had.
- `SpawnerGraph` is held as a `TStrongObjectPtr`, not a `TObjectPtr`/`UPROPERTY` - `SSolarOrbzMainPanel`
  is a Slate widget, not a `UObject`, so there's no reflected property to anchor the graph's GC root
  the way `USolarOrbzTerrainLayerStack::TerrainGraph` anchors the Terrain Graph Editor's graph. A
  `TStrongObjectPtr` is the standard tool for exactly this - "a plain C++ object needs to keep a
  `UObject` alive" - and releases automatically when the panel is destroyed (dock tab closed).
- A Planet Catalog section above the graph: an `SObjectPropertyEntryBox` to assign a `UDataTable`,
  and an **Apply Catalog Row** combo button listing that table's row names (`GetRowNames()`) -
  picking one calls `ApplyCatalogRow(RowName)`, which looks up the row and writes its fields
  directly into the five module config objects, then calls `IDetailsView::ForceRefresh()` so an
  already-open module's fields update immediately.

### The catalog row (`SolarOrbzPlanetCatalogRow.h`)

`FSolarOrbzPlanetCatalogRow : public FTableRowBase` mirrors the five module configs' fields exactly
- `RadiusMeters`/`VerticesPerMeter`/`MaxSubdivisions`/`bEnablePreviewCollision` plus
  `TSoftObjectPtr<USolarOrbzTerrainLayerStack> TerrainStack`/`TSoftObjectPtr<USolarOrbzBiomeStack>
  BiomeStack`/`TSoftObjectPtr<USolarOrbzClimateSimulationAsset> ClimateSimulation`/
  `TSoftObjectPtr<USolarOrbzCelestialBodyProfile> Profile`. Soft references so assigning a
  `UDataTable` doesn't force-load every referenced asset just by existing.

To build a catalog: Content Browser → Import a `.csv` → Row Struct =
`FSolarOrbzPlanetCatalogRow`. Object-reference columns need the asset's **full path** (e.g.
`/Game/SolarOrbz/Biomes/ASN_BIOME_DESERT.ASN_BIOME_DESERT`) - the engine's CSV→DataTable importer
resolves that into the soft reference automatically through its normal reflection-based column
import, the same way it would for any other object-reference column; nothing custom was written
for this. A biome column is meant to point at a prebuilt `USolarOrbzBiomeStack` asset (e.g.
`ASN_BIOME_DESERT`, `ASN_BIOME_OCEANDEEP`) - the row picks which one to assign, it doesn't describe
a biome stack inline.

**An empty/unset cell means "don't touch this field," not "clear it."** `ApplyCatalogRow` always
applies the four numeric Base Sphere fields (a catalog row is expected to fully specify those), but
only writes a module's object reference when the row's soft pointer for it is non-null - so a row
that only wants to drive Radius and Biome can leave Terrain Stack/Climate Simulation/Profile unset
without wiping out whatever's already assigned on those nodes.

## Known limitations / out of scope for this pass

- `SGraphEditor`/`IDetailsView`/`SObjectPropertyEntryBox`/`TStrongObjectPtr`-class code is
  engine-dependent UObject/Slate machinery, the same review-only verification bar as the Terrain
  Graph Editor and the chunked-terrain work - checked by careful reading against documented UE5.8
  APIs, not compiled or run.
- No validation on Apply - if a catalog row's `RadiusMeters` is nonsensical (e.g. 0), it's applied
  as-is; the same "no upper limit, performance is your only ceiling" posture
  `ASolarOrbzIcoSphereActor` already has.
- No batch/multi-planet spawning - this pass is one panel driving one preview actor at a time, same
  as before. A solar-system-scale "spawn N planets from N catalog rows" workflow is a reasonable
  follow-up but a separate, bigger design question (closer to the existing `ASNSolarSystemSpawner`
  pattern in the game-side codebase than to this plugin's own editor UI).
- Deleting a module node (Delete key) hides that module's Details-panel entry point until Ctrl+Z -
  there's no "restore node" affordance beyond undo.
