# SolarOrbz Terrain Graph Editor — Usage Guide

Companion to `Docs/SolarOrbzTerrainGraphEditor.md` (the design doc) - that doc explains *why* the
editor is built the way it is; this one is just "how do I actually use it." Confirmed building
clean against UE5.8 as of this writing. Nothing below has been exercised hands-on in the editor yet
- this is the first-use guide for exactly that pass, and a couple of interactions are flagged below
as genuinely unverified (not UE5.8-version hedges - those are resolved facts per this repo's
documentation standard - but honest "this is new code, tell me if it doesn't do what's described").

## 1. Placing a planet

The Terrain Graph Editor edits a `USolarOrbzTerrainLayerStack` asset, but that asset does nothing on
its own - it has to be assigned to a planet actor (`ASolarOrbzIcoSphereActor`) before you can see
what it does. That assignment now happens through the SolarOrbz panel's own graph - see
`Docs/SolarOrbzPlanetSpawnerGraph.md` for why it's a fixed star-shaped layout rather than a chain
like the Terrain Graph Editor's. If you already have a planet placed with a stack assigned, skip to
§2.

**Open the panel:** **Window → SolarOrbz**, or the **SolarOrbz** button on the Level Editor toolbar
next to Play.

**Reading the graph:** five module nodes - **Base Sphere**, **Terrain Stack**, **Biome Stack**,
**Climate Simulation**, **Profile** - each wired to a **Planet** sentinel. These connections are
fixed; there's nothing to rewire here (unlike the Terrain Graph Editor's chain). Click a node, its
fields fill the Details panel to the right - exactly the "select a node, edit the real object, no
sync step" pattern the Terrain Graph Editor uses.

| Node | Fields | What they do |
|---|---|---|
| **Base Sphere** | `Radius Meters`, `Vertices Per Meter`, `Max Subdivisions`, `Preview Collision` | Radius in meters (Earth is ~6,371,000, no upper limit - performance is the only ceiling). Vertices Per Meter is a density target, clamped by Max Subdivisions (hard-clamped at 11; keep it low, 6-9, for live editing - this actor is for a bounded preview/bake radius, not a full streaming planet). Preview Collision is off by default - add collision later on the baked mesh instead. |
| **Terrain Stack** | `Terrain Stack` | **Assign your `USolarOrbzTerrainLayerStack` here** - this is the asset the rest of this guide edits. |
| **Biome Stack** | `Biome Stack` | Optional - biome-specific detail layered on top of Terrain Stack. |
| **Climate Simulation** | `Climate Simulation` | Optional - feeds real temperature/moisture into biome masks. |
| **Profile** | `Profile` | Optional - a Planet/Star/Asteroid Profile asset; drives gravity, atmosphere, landmass counts. |

**Generating the preview:** click **Generate / Update Preview** below the graph. The first click
spawns an actor labeled `SolarOrbzIcoSphere` into the level and selects it, reading every node's
current values; later clicks update that same actor instead of spawning another. Unlike placing the
actor directly and editing its Details panel, nothing here regenerates live as you type - Generate
is always the step that actually applies the graph to the actor.

**Filling it in from a catalog row (optional, the fast path):** above the graph, assign a
**Data Table** of `FSolarOrbzPlanetCatalogRow` rows, then click **Apply Catalog Row** and pick a row
by name. This writes the row's Radius/Vertices Per Meter/Max Subdivisions/Collision into Base
Sphere unconditionally, and writes Terrain Stack/Biome Stack/Climate Simulation/Profile only where
the row actually specifies one (an empty cell leaves that node's existing assignment alone, rather
than clearing it) - then refreshes the Details panel so an already-open node's new values show up
immediately. Building the Data Table: Content Browser → Import a `.csv` → Row Struct =
`FSolarOrbzPlanetCatalogRow`. A Biome Stack (or Terrain Stack/Climate Simulation/Profile) cell needs
that asset's **full path** - e.g. a prebuilt biome stack named `ASN_BIOME_DESERT` goes in as
`/Game/SolarOrbz/Biomes/ASN_BIOME_DESERT.ASN_BIOME_DESERT` - the engine's own CSV→DataTable import
resolves that into the reference automatically. See the design doc for the full column list.

Once a preview looks right, **Bake To Static Mesh** (further down the same panel) writes it out to
a `UStaticMesh` asset at the **Package Path**/**Asset Name** you set there - that's the actual
deliverable for building solar-system assets.

The graph only holds staged values in memory for as long as the SolarOrbz panel stays open - closing
the tab and reopening it rebuilds a fresh, empty graph (same as the old flat panel losing its typed-in
Radius/etc. on reopen). Click Generate before closing the panel if you want a staged-but-not-yet-
generated planet to survive; once generated, the values live on the actor itself and are safe.

## 2. Opening a Terrain Layer Stack

A `USolarOrbzTerrainLayerStack` is still an ordinary `UPrimaryDataAsset` - nothing about *creating*
one changed. If you don't already have one: Content Browser → **Add** → **Miscellaneous → Data
Asset** → pick **Solar Orbz Terrain Layer Stack** as the class. It also now shows up filed under a
**SolarOrbz** category in that picker and in the Content Browser's type filter, instead of the
unfiled default.

**What's new:** double-click an existing stack asset and it now opens the **Terrain Graph Editor**
(a new tab layout: "Terrain Graph" + "Details") instead of the generic property grid. The generic
property grid is gone for this asset type - everything you used to edit there, you edit the same way
now, just reached by clicking a node instead of expanding an array entry (see §5).

## 3. Reading the graph

On open, the Terrain Graph tab shows your stack's layers as a left-to-right chain:

```
[Start] → [Noise Layer] → [Heightmap Layer] → [Erosion Layer] → [Output]
```

- **Start** (gray) - the base sphere before any layer runs. No properties, no input pin.
- **Output** (green) - the final combined height the planet actually samples. No properties, no
  output pin.
- Everything between them is one node per entry in `Layers`, **in the same order** - the chain's
  left-to-right order **is** the evaluation order (first enabled layer closest to Start is always an
  implicit Replace; everything after blends in via its own Blend Mode/Strength/Mask, same as before).
- A node's title is its layer type ("Noise Layer", "Heightmap Layer", etc.) - the same display names
  already used throughout this plugin.
- First time opening a stack that predates this editor, nodes lay out in an evenly-spaced row left to
  right. Rearrange them however you like - your layout is remembered per-asset (saved in the stack's
  own `EditorNodePositions`, so it persists across editor sessions once you save the asset).

## 4. Adding a layer

There's no right-click "Add Node" on empty canvas in this version - use the **Add Layer** button
above the graph canvas instead. Click it, pick a layer type from the dropdown (every concrete layer
type in the plugin is listed - Noise, Planetary Noise, Heightmap, Stamp, Erosion, Terrace, Canyon,
Continent). The new node appears **at the end of the chain, just before Output** with default
property values.

If you need it somewhere other than the end, see §6 (reordering) right after.

## 5. Editing a layer's properties

Click a node. The **Details** tab (right side) fills with that exact layer's properties - the same
fields you'd see expanding it in the old array widget (`Strength`, `Blend Mode`, `Mask`, plus
whatever's specific to that layer type: `AmplitudeMeters`, `HeightmapTexture`, `TalusAngleDegrees`,
etc.). Edit there as normal; it's the real asset data, not a copy, so no extra save/sync step.

Clicking **Start** or **Output** clears the Details tab - they're sentinels, not layers, and have no
properties of their own.

## 6. Reordering layers

The chain only allows one connection per pin, so reordering means **dragging a new connection onto
a pin that already has one** - the old connection breaks automatically the moment the new one is
made. There's no dedicated "move up/move down" button in this version; you rewire by hand.

Worked example - swapping the first two layers in `Start → A → B → C → Output` to get
`Start → B → A → C → Output`:

1. Drag from **Start's** output pin to **B's** input pin. (This single drag breaks two old links at
   once - `Start→A` and `A→B` - since both pins you touched already had connections. Result: `A` is
   now floating, disconnected on both sides.)
2. Drag from **B's** output pin to **A's** input pin. (Breaks `B→C`. Result: `Start→B→A`, and `C` is
   now floating on its input side.)
3. Drag from **A's** output pin to **C's** input pin. Result: `Start→B→A→C→Output` - done.

This is a bit fiddly for anything beyond a short hop - moving a layer several positions can take
several drags. If that turns out to be annoying in practice, say so; a "move up/down" context action
on the node is a reasonable follow-up and doesn't touch anything else in this system.

## 7. Removing a layer

Select the node and press **Delete**. *(Flagged as unverified: standard `SGraphEditor` keyboard
delete should just work here, but this is the one interaction in this editor I'd most want
confirmed on first hands-on pass - if Delete does nothing, tell me and I'll wire an explicit delete
command rather than relying on the graph's default.)* Start/Output can't be deleted - they're not
"in" `Layers` to begin with.

## 8. Saving

Ctrl+S / the editor's Save button, same as any other asset. Every structural edit (add a layer,
reconnect, delete) is already written back into the real `Layers` array **immediately**, not just on
save - save just persists that array (and your node layout) to disk as usual. You can close and
reopen the stack mid-edit without losing a reorder, in other words; save is only about disk
persistence, not about "committing" the graph edits.

## 9. If something looks wrong

Open **Window → Developer Tools → Output Log** and filter for `LogSolarOrbzTerrainGraph` - every
rebuild, compile-back-into-`Layers`, connection decision, and toolkit action logs there, including
warnings for the cases most likely to actually go wrong. (For the planet spawner graph in §1 instead,
filter for `LogSolarOrbzPlanetSpawner` - it logs the Data Table/Apply Row path and Generate clicks
the same way.)

| Symptom | What to check in the log |
|---|---|
| A layer you added/reordered doesn't seem to affect the planet | Look for a `CompileToLayers: writing N layer(s)` line after your edit - does N match what you expect? A `"chain from Start never reached Output"` warning means the chain was broken when it compiled (mid-drag, or a node got orphaned) - `Layers` was truncated to whatever it *did* reach. |
| The graph looks different from what you expect on reopening | `RebuildFromLayers: rebuilding <asset> from N layer(s)` - confirms what it rebuilt from. If N doesn't match the array you expect, the asset's actual `Layers` content is the thing to check (e.g. in source control history), not the graph. |
| Add Layer's dropdown is empty | A `GetDerivedClasses found no concrete USolarOrbzTerrainLayer subclasses` warning means something's wrong with module loading, not this editor specifically - worth a full rebuild. |
| The editor won't open at all / double-click still shows the old property grid | Check for a `USolarOrbzTerrainLayerStackAssetDefinition::OpenAssets` line - if it's not there at all, the asset definition likely isn't registering; a restart of the editor (not just a hot-reload) is worth trying first. |

## 10. Known limitations (carried over from the design doc)

- No branching - this is a straight chain, matching what `Layers` already was. See the design doc's
  "why a strict chain" section if you want the reasoning.
- No native right-click "Add Node" menu - the toolbar button in §4 does the same job.
- No per-node preview thumbnails - you still need to look at the actual planet to see a layer's
  effect.

These are all unchanged from the design doc's Phase 3 list, not new gaps introduced by this guide.
