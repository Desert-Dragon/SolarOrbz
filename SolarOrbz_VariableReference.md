# SolarOrbz — Variable Reference

Every editor-exposed variable in the plugin, grouped by file. "Location" gives
the header where it's declared. Units are UE convention (cm) unless noted;
as of this doc, **Radius is the one exception — it's authored in meters**
and converted to cm internally at the point it reaches the generator.

> ⚠️ **Unit inconsistency to be aware of**: `RadiusMeters` is meters, but
> every height-producing field below it (Noise `Amplitude`, Heightmap
> `MinHeight`/`MaxHeight`, Stamp `Amplitude`, Climate Mask `Elevation`
> range) is still in **centimeters** (plain UE units). This wasn't
> unified in this pass — only Radius was asked to move to meters. Worth
> deciding later whether to convert everything to meters for consistency.

---

## SolarOrbzIcoSphereActor.h
*The editor preview/bake actor — the top-level object you place in a level.*

| Variable | Type | Default | Range | Description |
|---|---|---|---|---|
| `RadiusMeters` | `float` | `1000.0` | Min `0.01`, **no upper limit** | Sphere radius, authored in **meters**. Converted to cm (`× 100`) once, at the top of `RegenerateMesh()`, before reaching the generator. |
| `VerticesPerMeter` | `float` | `1.0` | Min `0.001`, **no upper limit** | Target linear vertex density along the surface. Drives the subdivision level the generator picks. |
| `MaxSubdivisions` | `int32` | `6` | Min `0`, **no upper limit** | Hard ceiling on subdivision level regardless of requested density. Each +1 is roughly ×4 triangle count — watch the panel's stats line before pushing this high. |
| `TerrainStack` | `TObjectPtr<USolarOrbzTerrainLayerStack>` | `nullptr` | — | Optional. Base terrain recipe (procedural + authored layers), applied as Pass A of displacement. |
| `BiomeStack` | `TObjectPtr<USolarOrbzBiomeStack>` | `nullptr` | — | Optional. Biome-specific detail layered on top as Pass B, masked by climate/composite conditions. |
| `bShowBiomeDebugColors` | `bool` | `false` | — | Toggles per-vertex biome debug coloring (dominant biome's `PreviewColor`). Requires `DebugBiomeMaterial` to actually see it. |
| `DebugBiomeMaterial` | `TObjectPtr<UMaterialInterface>` | `nullptr` | — | Unlit material with VertexColor → Emissive wired up; applied to the PMC when debug colors are on. |
| `DefaultMaterial` | `TObjectPtr<UMaterialInterface>` | `nullptr` | — | Material restored when debug colors are toggled off. Leave unset to fall back to the engine default. |
| `bEnablePreviewCollision` | `bool` | `false` | — | Builds simple collision on the live PMC preview. Independent of the bake, which always builds simple collision regardless. |
| `BakePackagePath` | `FString` | `"/Game/SolarOrbz/Meshes"` | — | Content-browser folder the baked `UStaticMesh` is written to. |
| `BakeAssetName` | `FString` | `"SM_IcoSphere"` | — | Asset name for the baked static mesh. |
| `ProcMesh` | `TObjectPtr<UProceduralMeshComponent>` | (created in constructor) | `VisibleAnywhere`, not editable | The actor's root component; holds the live preview geometry. |

**Read-only accessors** (not editable, reflect the last `RegenerateMesh()` call): `GetLastSubdivisionLevelUsed()`, `GetPreviewVertexCount()`, `GetPreviewTriangleCount()`.

---

## SolarOrbzMainPanel.h
*The Slate UI panel content of the plugin's docking tab. Plain widget members — not `UPROPERTY`, not saved anywhere, just staging values pushed onto the actor when you click Generate.*

| Variable | Type | Default | Description |
|---|---|---|---|
| `RadiusMeters` | `float` | `1000.0` | Mirrors the actor's field while staged in the UI. |
| `VerticesPerMeter` | `float` | `1.0` | " |
| `MaxSubdivisions` | `int32` | `6` | " |
| `bEnablePreviewCollision` | `bool` | `false` | " |
| `BakePackagePath` | `FString` | `"/Game/SolarOrbz/Meshes"` | " |
| `BakeAssetName` | `FString` | `"SM_IcoSphere"` | " |
| `PreviewActor` | `TWeakObjectPtr<ASolarOrbzIcoSphereActor>` | `nullptr` | The actor spawned by Generate; re-used on subsequent clicks instead of spawning duplicates. |

---

## SolarOrbzIcoSphereGenerator.h
*Static utility class — no editor-exposed variables (not a `UCLASS`). Listed here because these are the underlying API parameters every actor property above ultimately maps onto.*

| Parameter | Type | Meaning |
|---|---|---|
| `Radius` | `float` | Sphere radius in **UE units (cm)** — the actor converts `RadiusMeters × 100` before calling in. |
| `VerticesPerMeter` | `float` | Same meaning as the actor property. |
| `MaxSubdivisions` | `int32` | Same meaning as the actor property; used as the clamp ceiling for the level the density calculation picks. |

---

## SolarOrbzTerrainLayer.h
*Abstract base for every terrain layer type.*

| Variable | Type | Default | Range | Description |
|---|---|---|---|---|
| `bEnabled` | `bool` | `true` | — | Skips this layer entirely when off. |
| `BlendMode` | `ESolarOrbzTerrainBlendMode` | `Add` | `Add / Subtract / Multiply / Max / Min / Replace` | How this layer's output combines with the accumulated result of layers before it in the stack. |
| `Weight` | `float` | `1.0` | — | Multiplies this layer's raw output before blending. |

---

## SolarOrbzNoiseTerrainLayer.h
*Fractal (multi-octave) Perlin noise — procedural terrain.*

| Variable | Type | Default | Range | Description |
|---|---|---|---|---|
| `Seed` | `int32` | `0` | — | Randomizes the pattern without changing its statistical character. |
| `Octaves` | `int32` | `5` | `1`–`10` | Number of fractal layers summed together. |
| `Frequency` | `float` | `2.0` | Min `0.01` | Base frequency, roughly "cycles per trip around the sphere." Small = continents, large = fine roughness. |
| `Lacunarity` | `float` | `2.0` | Min `1.0` | Frequency multiplier per octave. |
| `Persistence` | `float` | `0.5` | `0.0`–`1.0` | Amplitude multiplier per octave. |
| `Amplitude` | `float` | `500.0` (cm) | — | Final height scale — the peak-to-peak range the fractal sum maps onto. |
| `WarpStrength` | `float` | `0.0` | Min `0.0` | Domain warp strength; 0 disables it. |
| `WarpFrequency` | `float` | `1.0` | Min `0.01`, only shown if `WarpStrength > 0` | Frequency of the warp field. |

---

## SolarOrbzHeightmapTerrainLayer.h
*Authored equirectangular heightmap (real-world DEMs).*

| Variable | Type | Default | Description |
|---|---|---|---|
| `HeightmapTexture` | `TObjectPtr<UTexture2D>` | `nullptr` | Equirectangular grayscale heightmap. X = longitude, Y = latitude (north pole at top). Import with compression **None**. |
| `MinHeight` | `float` | `-1,100,000` (cm ≈ -11,000 m, Mariana Trench) | World-space height that black (0.0) maps to. |
| `MaxHeight` | `float` | `884,800` (cm ≈ 8,848 m, Everest) | World-space height that white (1.0) maps to. |

Internally uses a `mutable FSolarOrbzTextureHeightSampler Sampler` (see below) — not itself editor-exposed.

---

## SolarOrbzStampTerrainLayer.h
*Places one feature (heightmap or procedural dome/crater) at a specific point on the sphere.*

| Variable | Type | Default | Range | Description |
|---|---|---|---|---|
| `Latitude` | `float` | `0.0` | `-90` to `90` | Stamp center, degrees. 90 = north pole. |
| `Longitude` | `float` | `0.0` | `-180` to `180` | Stamp center, degrees. |
| `AngularRadius` | `float` | `15.0` | `0.1` to `180` | How far the stamp reaches, in degrees of surface arc. |
| `EdgeFalloff` | `float` | `0.25` | `0.0`–`1.0` | Fraction of `AngularRadius` over which the stamp fades out at its edge. |
| `StampRotationDegrees` | `float` | `0.0` | `-180` to `180` | Rotates the stamp around its own center (heightmap mode only). |
| `StampHeightmap` | `TObjectPtr<UTexture2D>` | `nullptr` | — | Optional. Sampled via local orthographic projection (no seam concerns). Leave unset for procedural dome/crater. |
| `Amplitude` | `float` | `1000.0` (cm) | — | Peak height — the dome/crater extremum, or the heightmap's white value. |
| `bCrater` | `bool` | `false` | Only shown without a heightmap | Carves down instead of domes up. |
| `CraterRimHeight` | `float` | `0.3` | Min `0.0`, only shown with `bCrater` and no heightmap | Raised rim at the crater edge, as a fraction of `Amplitude`. 0 disables it. |

Internally uses a `mutable FSolarOrbzTextureHeightSampler Sampler` — not itself editor-exposed.

---

## SolarOrbzTerrainLayerStack.h
*Data asset — the ordered, saveable recipe combining layers.*

| Variable | Type | Default | Description |
|---|---|---|---|
| `Layers` | `TArray<TObjectPtr<USolarOrbzTerrainLayer>>` (Instanced) | empty | Ordered stack of Noise/Heightmap/Stamp layers, composited top-to-bottom via each layer's `BlendMode`. |

---

## SolarOrbzTextureHeightSampler.h
*Plain (non-`UObject`) helper struct shared by Heightmap and Stamp layers — no editor-exposed fields, purely internal caching.*

| Field | Type | Purpose |
|---|---|---|
| `CachedHeights01` | `TArray<float>` | Decoded grayscale pixel data, 0..1, row-major. |
| `CachedWidth` / `CachedHeight` | `int32` | Cached texture dimensions. |
| `CachedTexture` | `TWeakObjectPtr<UTexture2D>` | Identifies which texture the cache belongs to, so it re-decodes only when the assigned texture changes. |

---

## SolarOrbzBiomeMask.h
*Base mask class + the per-point context passed to every mask and to biome terrain detail evaluation.*

`USolarOrbzBiomeMask` (abstract base) has no properties of its own.

**`FSolarOrbzBiomeSampleContext`** (read-only at runtime — built by the actor each pass, not authored):

| Field | Type | Description |
|---|---|---|
| `UnitDirection` | `FVector` | Point's position on the base unit sphere (pristine, pre-displacement direction). |
| `UV` | `FVector2D` | The mesh's spherical UV at this point. |
| `Elevation` | `float` (cm) | Height above/below base radius after the base terrain stack. |
| `Slope` | `float` | `0` flat .. `1` vertical cliff. |

---

## SolarOrbzClimateBiomeMask.h
*Whittaker-diagram-style classifier: elevation / latitude / slope / moisture, each a soft-edged range, multiplied together.*

**`FSolarOrbzMaskRange`** (reused four times below):

| Field | Type | Default | Description |
|---|---|---|---|
| `bEnabled` | `bool` | `false` | Disabled = always passes (no restriction on that axis). |
| `Min` | `float` | `0.0` | Range floor. |
| `Max` | `float` | `1.0` | Range ceiling. |
| `Falloff` | `float` | `0.1` | Min `0.0`. Transition softness at each edge; 0 = hard cutoff. |

**`USolarOrbzClimateBiomeMask`**:

| Variable | Type | Default | Description |
|---|---|---|---|
| `Elevation` | `FSolarOrbzMaskRange` | disabled | Elevation range, cm, relative to base radius. |
| `Latitude` | `FSolarOrbzMaskRange` | disabled | `0` equator .. `1` pole — temperature stand-in. |
| `Slope` | `FSolarOrbzMaskRange` | disabled | `0` flat .. `1` vertical. |
| `Moisture` | `FSolarOrbzMaskRange` | disabled | `0..1`, driven by a noise field standing in for rainfall/humidity. |
| `MoistureSeed` | `int32` | `0` | Only shown if `Moisture.bEnabled`. |
| `MoistureFrequency` | `float` | `1.5` | Min `0.01`, only shown if `Moisture.bEnabled`. |

---

## SolarOrbzCompositeBiomeMask.h
*Combines other Mask Preset assets — the actual mechanism behind compound biomes.*

| Variable | Type | Default | Description |
|---|---|---|---|
| `Masks` | `TArray<TObjectPtr<USolarOrbzBiomeMaskPreset>>` | empty | Child mask presets to combine. Empty = always passes. |
| `CombineMode` | `ESolarOrbzMaskCombineMode` | `Multiply` | `Multiply` (intersection — all must apply), `Min`, `Max` (union — any is enough), `Average`. |

---

## SolarOrbzBiomeMaskPreset.h
*Reusable, standalone asset wrapping one mask — create via right-click → Miscellaneous → Data Asset.*

| Variable | Type | Default | Description |
|---|---|---|---|
| `RootMask` | `TObjectPtr<USolarOrbzBiomeMask>` (Instanced) | `nullptr` | Class-picker dropdown — choose Climate Mask, Composite Mask, etc. This is where the actual condition is authored. |

---

## SolarOrbzBiome.h
*Reusable biome preset asset.*

**`FSolarOrbzBiomeScatterEntry`** (placeholder for the future PCG/PCGEx hookup):

| Field | Type | Default | Description |
|---|---|---|---|
| `Tag` | `FName` | none | Matched against PCG graph settings later, e.g. `"Rock_Large"`. |
| `DensityPerSquareMeter` | `float` | `0.01` | Min `0.0`. |
| `MinSlope` | `float` | `0.0` | `0.0`–`1.0`. |
| `MaxSlope` | `float` | `1.0` | `0.0`–`1.0`. |

**`USolarOrbzBiome`**:

| Variable | Type | Default | Description |
|---|---|---|---|
| `PreviewColor` | `FLinearColor` | White | Used for editor visualization (mask previews, biome debug colors). |
| `TerrainDetail` | `TObjectPtr<USolarOrbzTerrainLayerStack>` | `nullptr` | This biome's own extra terrain layers, blended in wherever its mask is active. |
| `ScatterEntries` | `TArray<FSolarOrbzBiomeScatterEntry>` | empty | What this biome scatters and how densely — not yet consumed by anything (PCG hookup pending). |

---

## SolarOrbzBiomeStack.h
*Ordered, masked biome layers — the "Photoshop layers" list painted onto a planet.*

**`FSolarOrbzBiomeLayerEntry`**:

| Field | Type | Default | Description |
|---|---|---|---|
| `Biome` | `TObjectPtr<USolarOrbzBiome>` | `nullptr` | The biome this layer applies. |
| `Mask` | `TObjectPtr<USolarOrbzBiomeMaskPreset>` | `nullptr` | Where it applies. Unset = "always applies." |
| `Opacity` | `float` | `1.0` | `0.0`–`1.0`. Overall strength multiplier, independent of the mask. |

**`USolarOrbzBiomeStack`**:

| Variable | Type | Default | Description |
|---|---|---|---|
| `Layers` | `TArray<FSolarOrbzBiomeLayerEntry>` | empty | `Layers[0]` = bottom of the stack; last entry = topmost / highest priority. |

---

## Quick index — "I want to change X, where do I look?"

| I want to... | Edit... |
|---|---|
| Change the planet's overall size | `RadiusMeters` on the actor (meters) |
| Change mesh density/resolution | `VerticesPerMeter` / `MaxSubdivisions` on the actor |
| Add rolling procedural terrain | A `SolarOrbzNoiseTerrainLayer` inside a `SolarOrbzTerrainLayerStack` |
| Import a real-world DEM (Earth, Mars) | A `SolarOrbzHeightmapTerrainLayer` |
| Place one named feature at an exact spot | A `SolarOrbzStampTerrainLayer` |
| Define "where" a biome applies | A `SolarOrbzBiomeMaskPreset` (Climate or Composite) |
| Build a compound biome ("Icy Chemical Mountain") | A `SolarOrbzCompositeBiomeMask` combining two `Climate` presets, `CombineMode = Multiply` |
| Give a biome its own extra terrain roughness | `TerrainDetail` on the `SolarOrbzBiome` asset |
| See biome regions in the viewport | `bShowBiomeDebugColors` + `DebugBiomeMaterial` on the actor |
| Change where the baked mesh saves to | `BakePackagePath` / `BakeAssetName` on the actor |
