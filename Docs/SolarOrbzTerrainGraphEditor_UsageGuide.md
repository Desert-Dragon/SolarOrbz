# SolarOrbz Terrain Graph Editor — Usage Guide

Companion to `Docs/SolarOrbzTerrainGraphEditor.md` (the design doc) - that doc explains *why* the
editor is built the way it is; this one is just "how do I actually use it." Confirmed building
clean against UE5.8 as of this writing. Nothing below has been exercised hands-on in the editor yet
- this is the first-use guide for exactly that pass, and a couple of interactions are flagged below
as genuinely unverified (not UE5.8-version hedges - those are resolved facts per this repo's
documentation standard - but honest "this is new code, tell me if it doesn't do what's described").

## 1. Opening a Terrain Layer Stack

A `USolarOrbzTerrainLayerStack` is still an ordinary `UPrimaryDataAsset` - nothing about *creating*
one changed. If you don't already have one: Content Browser → **Add** → **Miscellaneous → Data
Asset** → pick **Solar Orbz Terrain Layer Stack** as the class. It also now shows up filed under a
**SolarOrbz** category in that picker and in the Content Browser's type filter, instead of the
unfiled default.

**What's new:** double-click an existing stack asset and it now opens the **Terrain Graph Editor**
(a new tab layout: "Terrain Graph" + "Details") instead of the generic property grid. The generic
property grid is gone for this asset type - everything you used to edit there, you edit the same way
now, just reached by clicking a node instead of expanding an array entry (see §4).

## 2. Reading the graph

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

## 3. Adding a layer

There's no right-click "Add Node" on empty canvas in this version - use the **Add Layer** button
above the graph canvas instead. Click it, pick a layer type from the dropdown (every concrete layer
type in the plugin is listed - Noise, Planetary Noise, Heightmap, Stamp, Erosion, Terrace, Canyon,
Continent). The new node appears **at the end of the chain, just before Output** with default
property values.

If you need it somewhere other than the end, see §5 (reordering) right after.

## 4. Editing a layer's properties

Click a node. The **Details** tab (right side) fills with that exact layer's properties - the same
fields you'd see expanding it in the old array widget (`Strength`, `Blend Mode`, `Mask`, plus
whatever's specific to that layer type: `AmplitudeMeters`, `HeightmapTexture`, `TalusAngleDegrees`,
etc.). Edit there as normal; it's the real asset data, not a copy, so no extra save/sync step.

Clicking **Start** or **Output** clears the Details tab - they're sentinels, not layers, and have no
properties of their own.

## 5. Reordering layers

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

## 6. Removing a layer

Select the node and press **Delete**. *(Flagged as unverified: standard `SGraphEditor` keyboard
delete should just work here, but this is the one interaction in this editor I'd most want
confirmed on first hands-on pass - if Delete does nothing, tell me and I'll wire an explicit delete
command rather than relying on the graph's default.)* Start/Output can't be deleted - they're not
"in" `Layers` to begin with.

## 7. Saving

Ctrl+S / the editor's Save button, same as any other asset. Every structural edit (add a layer,
reconnect, delete) is already written back into the real `Layers` array **immediately**, not just on
save - save just persists that array (and your node layout) to disk as usual. You can close and
reopen the stack mid-edit without losing a reorder, in other words; save is only about disk
persistence, not about "committing" the graph edits.

## 8. If something looks wrong

Open **Window → Developer Tools → Output Log** and filter for `LogSolarOrbzTerrainGraph` - every
rebuild, compile-back-into-`Layers`, connection decision, and toolkit action logs there, including
warnings for the cases most likely to actually go wrong:

| Symptom | What to check in the log |
|---|---|
| A layer you added/reordered doesn't seem to affect the planet | Look for a `CompileToLayers: writing N layer(s)` line after your edit - does N match what you expect? A `"chain from Start never reached Output"` warning means the chain was broken when it compiled (mid-drag, or a node got orphaned) - `Layers` was truncated to whatever it *did* reach. |
| The graph looks different from what you expect on reopening | `RebuildFromLayers: rebuilding <asset> from N layer(s)` - confirms what it rebuilt from. If N doesn't match the array you expect, the asset's actual `Layers` content is the thing to check (e.g. in source control history), not the graph. |
| Add Layer's dropdown is empty | A `GetDerivedClasses found no concrete USolarOrbzTerrainLayer subclasses` warning means something's wrong with module loading, not this editor specifically - worth a full rebuild. |
| The editor won't open at all / double-click still shows the old property grid | Check for a `USolarOrbzTerrainLayerStackAssetDefinition::OpenAssets` line - if it's not there at all, the asset definition likely isn't registering; a restart of the editor (not just a hot-reload) is worth trying first. |

## 9. Known limitations (carried over from the design doc)

- No branching - this is a straight chain, matching what `Layers` already was. See the design doc's
  "why a strict chain" section if you want the reasoning.
- No native right-click "Add Node" menu - the toolbar button in §3 does the same job.
- No per-node preview thumbnails - you still need to look at the actual planet to see a layer's
  effect.

These are all unchanged from the design doc's Phase 3 list, not new gaps introduced by this guide.
