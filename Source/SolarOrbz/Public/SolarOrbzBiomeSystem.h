// SolarOrbz - Biome System subsystem. Combines the mask base class/sample context, the Climate
// and Composite mask types, the Biome asset (which now owns its own mask directly - see below),
// and the ordered Biome Stack into one file. Grouped together because they form one tightly-
// coupled system: masks answer "how strongly does a biome apply here" (0..1) given a point's
// climate/shape inputs, biomes bundle what to actually paint when their mask matches, and the
// stack combines many biomes Photoshop-style across a whole planet.
//
// A biome's mask is now embedded directly on the Biome asset (Instanced, same pattern as Terrain
// Layers on a Terrain Layer Stack) instead of living in a separate standalone Mask Preset asset -
// one asset to author per biome instead of two. Reuse of a mask condition across multiple biomes
// still works, just differently: a Composite Mask now references other Biome assets directly and
// combines their embedded masks, rather than referencing separate preset files. So "Icy Chemical
// Mountain" is built by creating a Biome whose own Mask is a Composite Mask listing your existing
// "Icy" and "Mountain" biomes - reusing their conditions without redefining them.
//
// Mask selector inputs are modeled on World Creator's own Distribution system (verified against its
// public features page/real editor screenshots - not decompiled or copied, the input categories and
// panel shape are what's referenced, not any of its code or assets), which splits mask inputs into
// Noise/Layer/Terrain categories. Climate Mask already covered most of its "Terrain" category
// (Elevation=Height, Slope) plus true climate signals (Temperature/Moisture/Latitude) it doesn't
// have at all; a later pass added Sun Exposure (WC's "Sun Direction") to Climate Mask, and a Noise
// Mask for WC's whole "Noise" category. This pass covers 4 of WC's 9 "Layer" category entries - Area
// Mask, Polygon Mask, Path Mask, Border Blend Mask - the ones that are placed geometry (typed
// Latitude/Longitude points), not interactive authoring tools. The other 5 (Mask paint/Model/Rivers/
// Simulation/Filter Change) are deliberately NOT started - see the comment block directly above
// those 4 classes below for why each one specifically doesn't fit this same pattern.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "SolarOrbzTerrainLayers.h"
#include "SolarOrbzBiomeSystem.generated.h"

class USolarOrbzBiome;

// ================================================================================================
// FSolarOrbzBiomeSampleContext / USolarOrbzBiomeMask - everything a mask (or a biome's terrain
// detail layer) might need to know about a point on the planet, and the base class every mask
// type below derives from. Masks are composable (see Composite Mask) so compound biomes like
// "Icy Chemical Mountain" fall out of stacking simple masks rather than needing a hardcoded
// category for every combination.
// ================================================================================================

/** Everything a mask (or a biome's terrain detail layer) might need to know about a point on the planet. */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzBiomeSampleContext
{
	GENERATED_BODY()

	/** Normalized direction from the planet center - the point's position on the base unit sphere. */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	FVector UnitDirection = FVector::UpVector;

	/** The mesh's spherical UV at this point (matches FSolarOrbzIcoSphereMeshData::UVs). */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	FVector2D UV = FVector2D::ZeroVector;

	/** Height above/below the base radius after the base terrain stack has been applied, in UE units (cm). */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	float Elevation = 0.0f;

	/** 0 = flat ground, 1 = vertical cliff face. Derived from how far the surface normal has tilted away from UnitDirection. */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	float Slope = 0.0f;

	/**
	 * Absolute temperature in Kelvin, not normalized. Only meaningful when bHasClimateData is true -
	 * i.e. the actor has a ClimateSimulation asset assigned. Otherwise left at its default and masks
	 * fall back to a plain latitude proxy.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome", meta = (Units = "Kelvin"))
	float Temperature = 288.0f;

	/**
	 * 0 = driest, 1 = wettest (oceans are always 1.0). Only meaningful when bHasClimateData is true;
	 * comes from a whole-planet wind/orographic simulation rather than a per-point noise field, so
	 * real rain shadows show up behind mountain ranges. Otherwise left at its default.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	float Moisture = 0.5f;

	/** True once Temperature/Moisture above have been filled in by a ClimateSimulation asset. False = no simulation is assigned; masks should use their own fallback logic instead. */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	bool bHasClimateData = false;

	/**
	 * Sea level, meters, same zero point Elevation is measured from (the planet's base radius).
	 * Comes straight from a ClimateSimulation asset's Sea Level when one is assigned - independent
	 * of bHasClimateData, since Sea Level is just an authored value, not something the simulation
	 * has to actually run to know. 0 (the default, matching ClimateSimulation's own default) when no
	 * ClimateSimulation asset is assigned. Elevation-based masks subtract this automatically, so
	 * moving Sea Level moves biome bands with it instead of leaving them anchored to the old coastline.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome", meta = (Units = "m"))
	float SeaLevel = 0.0f;

	/**
	 * Tangential height gradient at this point - X = toward local north, Y = toward local east (a
	 * right-handed tangent basis with UnitDirection as the outward axis), roughly rise-per-unit-
	 * tangential-distance along each. Only meaningful when a mask opted in via
	 * USolarOrbzBiomeMask::NeedsLocalGradient() (see USolarOrbzTerrainLayerStack::
	 * EstimateLocalGradientUpTo) - zero otherwise, same "not yet computed, don't trust it" convention
	 * Slope already uses when NeedsSlope() is false. Magnitude is Slope-like (steeper = larger); a
	 * mask that only cares which way a slope faces, not how steep, should normalize it itself.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	FVector2D LocalGradient = FVector2D::ZeroVector;
};

UCLASS(Abstract, EditInlineNew, DefaultToInstanced, BlueprintType)
class SOLARORBZ_API USolarOrbzBiomeMask : public UObject
{
	GENERATED_BODY()

public:
	/** Returns how strongly this mask applies at the given point, 0 (not at all) .. 1 (fully). */
	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const { return 1.0f; }

	/**
	 * True if GetWeight ever reads Context.Temperature/Context.Moisture for this specific instance's
	 * authored settings - i.e. needs a Climate Simulation grid to be meaningful. Lets a caller cheaply
	 * tell, once per regenerate rather than by probing every point, whether it's worth paying for
	 * climate data before evaluating this mask (see USolarOrbzTerrainLayerStack::AnyLayerNeedsClimateData).
	 * Default false - most masks don't touch these axes.
	 */
	virtual bool NeedsClimateData() const { return false; }

	/** Same idea as NeedsClimateData, for Context.Slope - which (unlike Elevation) isn't cheap to know mid-Pass-A, since it needs a finite-difference estimate rather than a real mesh normal. Default false. */
	virtual bool NeedsSlope() const { return false; }

	/** Same idea as NeedsSlope, for Context.LocalGradient - a direction, not just a magnitude, so it costs two finite-difference samples mid-Pass-A instead of Slope's one (see USolarOrbzTerrainLayerStack::EstimateLocalGradientUpTo). Default false. */
	virtual bool NeedsLocalGradient() const { return false; }
};

// ================================================================================================
// FSolarOrbzMaskRange / USolarOrbzClimateBiomeMask - classic Whittaker-diagram-style classifier:
// combines elevation, latitude/temperature, slope, and moisture, each as a soft-edged range,
// multiplied together. Soft edges mean adjacent biomes blend across a transition band instead of
// hard-cutting.
// ================================================================================================

/** A soft-edged [Min, Max] range: 1.0 inside, ramping to 0.0 over Falloff at each edge. Disabled = always 1.0 (no restriction on that axis). */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzMaskRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range")
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range", meta = (EditCondition = "bEnabled"))
	float Min = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range", meta = (EditCondition = "bEnabled"))
	float Max = 1.0f;

	/** How gradual the transition at the edges is, in the same units as Min/Max. 0 = hard cutoff. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range", meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float Falloff = 0.1f;

	float Evaluate(float Value) const;
};

UCLASS(EditInlineNew, meta = (DisplayName = "Climate Mask"))
class SOLARORBZ_API USolarOrbzClimateBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	/** Elevation range, meters, relative to Sea Level (not the raw base radius) - moves with Sea Level if you change it on your ClimateSimulation asset. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Elevation;

	/** 0 = equator, 1 = pole. Purely geometric - doesn't account for elevation. Prefer Temperature below once a ClimateSimulation asset is assigned; keep using this one for simple cases or when no simulation exists. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Latitude;

	/**
	 * Absolute temperature in Kelvin - not normalized. Simulated (latitude baseline minus elevation
	 * lapse rate) when the actor has a ClimateSimulation asset assigned; otherwise falls back to a
	 * generic Earth-like Lerp by latitude (288K equator .. 255K pole) so this axis is at least in the
	 * right ballpark without a simulation. Author the Min/Max below in Kelvin to match whatever your
	 * ClimateSimulation asset actually produces - this works equally for an icy moon or a Venus-hot world.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (Units = "Kelvin"))
	FSolarOrbzMaskRange Temperature;

	/** 0 = flat ground, 1 = vertical cliff face. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Slope;

	/**
	 * 0 = driest, 1 = wettest. Comes from a wind/orographic simulation (real rain shadows behind
	 * mountains) when the actor has a ClimateSimulation asset assigned; otherwise falls back to the
	 * low-frequency noise field below, standing in for rainfall/humidity until a simulation exists.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Moisture;

	/** Only used as the noise fallback when no ClimateSimulation asset is assigned upstream. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (EditCondition = "Moisture.bEnabled"))
	int32 MoistureSeed = 0;

	/** Only used as the noise fallback when no ClimateSimulation asset is assigned upstream. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (EditCondition = "Moisture.bEnabled", ClampMin = "0.01"))
	float MoistureFrequency = 1.5f;

	/**
	 * How directly this point's slope faces toward SunAzimuthDegrees below - 0 = faces directly away
	 * (the classic "shaded, snow/moisture lingers longer" side of a ridge), 1 = faces directly toward
	 * it (dries out and melts first), 0.5 on flat ground (no facing direction to speak of) or a slope
	 * facing exactly sideways. World Creator calls the equivalent terrain input "Sun Direction" -
	 * verified against its own public features page, not guessed. Costs two extra finite-difference
	 * height samples mid-Pass-A (see USolarOrbzBiomeMask::NeedsLocalGradient) - same cost class as
	 * Slope above, just doubled, since direction (not only steepness) is what this axis needs.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange SunExposure;

	/** Compass bearing the sun/key light comes from - 0 = local north, 90 = local east, matching Latitude's own geometric convention. A flat ground-plane bearing only; no elevation angle or day/night model. Only used when SunExposure.bEnabled. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (EditCondition = "SunExposure.bEnabled", ClampMin = "0.0", ClampMax = "360.0"))
	float SunAzimuthDegrees = 0.0f;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
	virtual bool NeedsClimateData() const override { return Temperature.bEnabled || Moisture.bEnabled; }
	virtual bool NeedsSlope() const override { return Slope.bEnabled; }
	virtual bool NeedsLocalGradient() const override { return SunExposure.bEnabled; }
};

// ================================================================================================
// USolarOrbzCompositeBiomeMask - combines other biomes' masks so compound biome variants ("Icy
// Chemical" + "Mountain") are built by stacking simple, reusable conditions rather than needing a
// dedicated class or enum entry per combination. Children are Biome asset references, so the same
// building-block biome (e.g. "Icy") can be reused as an input condition across many composites
// without redefining its mask.
// ================================================================================================
UENUM(BlueprintType)
enum class ESolarOrbzMaskCombineMode : uint8
{
	Multiply, // intersection - "all of these must apply" (this is what you want for compound biomes)
	Min,
	Max,      // union - "any of these applying is enough"
	Average,
};

UCLASS(EditInlineNew, meta = (DisplayName = "Composite Mask"))
class SOLARORBZ_API USolarOrbzCompositeBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	/** Other biomes whose masks combine to form this condition. Each one's own Mask is what's actually evaluated - this just reuses it, it doesn't duplicate it. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Composite")
	TArray<TObjectPtr<USolarOrbzBiome>> Biomes;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Composite")
	ESolarOrbzMaskCombineMode CombineMode = ESolarOrbzMaskCombineMode::Multiply;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
	virtual bool NeedsClimateData() const override;
	virtual bool NeedsSlope() const override;
	virtual bool NeedsLocalGradient() const override;
};

// ================================================================================================
// USolarOrbzNoiseBiomeMask - a pure procedural mask, independent of terrain shape or climate -
// World Creator's "Noise" Distribution category (verified against its own public features page: a
// 14-basis-type library used as a mask input, separate from its "Terrain" category's shape-derived
// signals). Reuses exactly the basis types a Noise Terrain Layer already supports
// (Perlin/Ridged/Billow/Value/Voronoi, SolarOrbzNoiseBasis in SolarOrbzTerrainLayers.h/.cpp) rather
// than the full 14 (Gabor/Phasor/Splatter/Wave/White/Worley/Dots/Fill/Simplex/NoiseValue aren't
// implemented anywhere in this plugin yet) - a deliberate scope-cut, not an oversight: adding a new
// basis type is a change to ESolarOrbzNoiseType itself, shared with every terrain-shaping Noise
// Layer too, not something to do piecemeal just for masking. Persistence/Lacunarity/domain warp are
// fixed (0.5/2.0/none) rather than separately authored fields - a mask weight doesn't need the full
// authoring surface a height-shaping layer does, just enough multi-octave variation to avoid a
// single flat basis reading too mechanically regular.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Noise Mask"))
class SOLARORBZ_API USolarOrbzNoiseBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	ESolarOrbzNoiseType NoiseType = ESolarOrbzNoiseType::Perlin;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.01"))
	float Frequency = 2.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "1", ClampMax = "8"))
	int32 Octaves = 3;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	int32 Seed = 0;

	/**
	 * Carves a band out of the raw noise field (remapped 0..1 from the basis's native -1..1) - same
	 * soft-edged [Min, Max] idea Climate Mask's own ranges use, so "only the top 30% of the noise
	 * field" is one enabled range rather than a second mask type. Disabled (default): the raw 0..1
	 * noise value is used directly as the weight, for a soft, fully-graduated blend instead of a
	 * carved band.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	FSolarOrbzMaskRange Threshold;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
};

// ================================================================================================
// USolarOrbzAreaBiomeMask / USolarOrbzPolygonBiomeMask / USolarOrbzPathBiomeMask /
// USolarOrbzBorderBlendBiomeMask - World Creator's "Layer" Distribution category: hand-placed
// geometric regions/lines, as opposed to Terrain's shape-derived signals or Noise's procedural
// field. Covers 4 of WC's 9 Layer entries (Area, Polygon, Path, Border Blend) - the ones that are
// pure placed-geometry math, authored by typing in an ordered list of Latitude/Longitude points,
// same spirit as ShapeStampNode's own placement convention in the standalone tool (and this file's
// existing USolarOrbzStampTerrainLayer, whose exact Latitude/Longitude -> direction formula these
// reuse verbatim, via SolarOrbzSphericalGeometry::DirectionFromLatLong below, for one consistent
// placement convention plugin-wide). WC's other 5 Layer entries - Mask (hand-painted, needs a real
// brush tool), Model (an imported mesh's own footprint), Rivers (flow-routed channel carving),
// Simulation (reads a water-flow/snow-melt simulation this plugin doesn't compute anywhere), Filter
// Change (reacts to a specific stack layer's own before/after delta, which would need a mask to see
// which stack index it's attached to - a bigger context-shape change) - are deliberately NOT
// attempted here: either real interactive editor tooling (viewport painting, a live brush) too risky
// to hand-write without ever running it in an actual UE5.8 editor, or blocked on a simulation/
// plumbing change that doesn't exist yet. Each is its own separate undertaking, same reasoning as
// last time's "Layer category deferred" note.
//
// Polygon/Path both need "is this point near a set of points on a sphere" math, which doesn't
// reduce to flat 2D geometry directly - solved by projecting every point (vertices and the query
// point alike) onto one shared local tangent plane via an azimuthal-equidistant projection centered
// on the shape's own average direction (exact same technique ShapeStampNode's Canyon archetype
// already uses in the standalone tool: the projected 2D distance between two points equals their
// true great-circle angular distance, in radians, for anything not spanning more than roughly a
// hemisphere from the projection center - an accepted approximation for a very large region, same
// trade-off already accepted there), then doing ordinary flat-2D point-in-polygon/point-to-segment
// math on the projected points. Deliberately NOT exact spherical trig (a cross-track/great-circle-
// segment-distance formula was drafted and then set aside in favor of this - reusing one already-
// reasoned-through projection technique across both new mask types is lower-risk than introducing a
// second, more intricate piece of hand-derived spherical geometry with no compiler to catch a sign
// error in either).
// ================================================================================================

UCLASS(EditInlineNew, meta = (DisplayName = "Area Mask"))
class SOLARORBZ_API USolarOrbzAreaBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	/** Where the area is centered, in degrees - same convention as USolarOrbzStampTerrainLayer's own Latitude/Longitude. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Area", meta = (ClampMin = "-90.0", ClampMax = "90.0"))
	float LatitudeDegrees = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Area", meta = (ClampMin = "-180.0", ClampMax = "180.0"))
	float LongitudeDegrees = 0.0f;

	/** How far the area reaches from its center, in degrees of arc along the sphere's surface. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Area", meta = (ClampMin = "0.01"))
	float RadiusDegrees = 10.0f;

	/** How gradual the transition at the edge is, in degrees. 0 = hard cutoff - same idea as FSolarOrbzMaskRange::Falloff. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Area", meta = (ClampMin = "0.0"))
	float FalloffDegrees = 2.0f;

	/** Weight outside the area instead of inside - "everywhere except here" rather than "only here". */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Area")
	bool bInvert = false;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
};

UCLASS(EditInlineNew, meta = (DisplayName = "Polygon Mask"))
class SOLARORBZ_API USolarOrbzPolygonBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	/**
	 * Ordered boundary points - X = Latitude degrees, Y = Longitude degrees, same convention as
	 * Area Mask above. Implicitly closed (the last point connects back to the first) - at least 3
	 * needed for a meaningful region; fewer always evaluates to 0 (outside everywhere).
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Polygon")
	TArray<FVector2D> VertexLatLongDegrees;

	/** How gradual the transition at the boundary is, in degrees. 0 = hard cutoff. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Polygon", meta = (ClampMin = "0.0"))
	float FalloffDegrees = 2.0f;

	/** Weight outside the polygon instead of inside. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Polygon")
	bool bInvert = false;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
};

UCLASS(EditInlineNew, meta = (DisplayName = "Path Mask"))
class SOLARORBZ_API USolarOrbzPathBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	/**
	 * Ordered points along the path - X = Latitude degrees, Y = Longitude degrees. NOT closed, unlike
	 * Polygon Mask above - a line, not a region. At least 2 needed; fewer always evaluates to 0.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Path")
	TArray<FVector2D> PointsLatLongDegrees;

	/** Half-width of the path's full-weight core, in degrees of arc. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Path", meta = (ClampMin = "0.0"))
	float WidthDegrees = 1.0f;

	/** How gradual the transition beyond Width is, in degrees. 0 = hard cutoff. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Path", meta = (ClampMin = "0.0"))
	float FalloffDegrees = 2.0f;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
};

UCLASS(EditInlineNew, meta = (DisplayName = "Border Blend Mask"))
class SOLARORBZ_API USolarOrbzBorderBlendBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	/**
	 * Any other mask type (an Area/Polygon/Climate/... Mask, picked from the same dropdown a Biome's
	 * own Mask uses) - Border Blend reads its weight but doesn't use it directly; see BandHalfWidth.
	 */
	UPROPERTY(EditAnywhere, Instanced, Category = "SolarOrbz|BorderBlend")
	TObjectPtr<USolarOrbzBiomeMask> ReferenceMask;

	/**
	 * Border Blend's own weight is 1.0 exactly where ReferenceMask's weight crosses 0.5 (its own
	 * transition edge, wherever that falls), fading to 0.0 once ReferenceMask's weight is more than
	 * BandHalfWidth away from 0.5 in either direction - a ring around whatever boundary
	 * ReferenceMask itself defines, rather than the region on one side of it. 0 (unset/no Reference
	 * Mask) always evaluates to 0.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|BorderBlend", meta = (ClampMin = "0.001", ClampMax = "0.5"))
	float BandHalfWidth = 0.1f;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
	virtual bool NeedsClimateData() const override { return ReferenceMask && ReferenceMask->NeedsClimateData(); }
	virtual bool NeedsSlope() const override { return ReferenceMask && ReferenceMask->NeedsSlope(); }
	virtual bool NeedsLocalGradient() const override { return ReferenceMask && ReferenceMask->NeedsLocalGradient(); }
};

// ================================================================================================
// FSolarOrbzBiomeScatterEntry / USolarOrbzBiome - a reusable bundle: the mask deciding where it
// applies, optional extra terrain detail (blended in wherever that mask is active), and scatter
// definitions that feed a real PCG/PCGEx hookup - see
// USolarOrbzBiomeStack::ExportScatterEntriesToPCGExMeshCollectionJson below. Store these as assets
// and reuse them across planets, the same way World Creator's biome presets work.
// ================================================================================================

/**
 * One scatter rule: what to place (ScatterMesh), how densely, and within what slope range.
 * ScatterMesh is exported directly into a real PCGEx Mesh Collection (see
 * USolarOrbzBiomeStack::ExportScatterEntriesToPCGExMeshCollectionJson) - Tag becomes that
 * collection entry's Category, so a PCGEx graph's own filter/distribute nodes can still group and
 * branch on it exactly as originally intended, now with a real asset behind each entry instead of
 * only a name. DensityPerSquareMeter/MinSlope/MaxSlope describe PLACEMENT, not the asset itself -
 * PCGEx models that as a separate concern from the collection entry (its own Distribute-family
 * node settings, not a collection entry field), so these three aren't part of that JSON export;
 * they're reported alongside it (see that function's own comment) for hand-entry into whichever
 * distribute/density node a PCGEx graph actually uses.
 */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzBiomeScatterEntry
{
	GENERATED_BODY()

	/** Matched against PCG graph settings later - e.g. "Rock_Large", "Tree_Pine", "Wildlife_Deer". Exported as this entry's Category in a PCGEx Mesh Collection. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter")
	FName Tag;

	/** What to scatter. Unset entries are skipped (with a warning) by ExportScatterEntriesToPCGExMeshCollectionJson - nothing to export without one. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter")
	TSoftObjectPtr<class UStaticMesh> ScatterMesh;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0"))
	float DensityPerSquareMeter = 0.01f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSlope = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxSlope = 1.0f;
};

/**
 * DataTable row struct matching the standalone Continent Generator tool's
 * "<out-prefix>_pcg_points.csv" export column-for-column (see
 * Tools/StandaloneContinentGenerator/README.md's "PCG scatter-point dataset" section) - import that
 * CSV as a DataTable of this row type to get one DataTable row per coarse attribute-sample point on
 * the baked planet, ready for a PCG graph's Data Table point source. Fields deliberately mirror
 * FSolarOrbzBiomeSampleContext's own shape (Elevation/Slope/Temperature/Moisture) rather than
 * introducing a parallel convention, and BiomeName is meant to be matched against
 * FSolarOrbzBiomeScatterEntry::Tag by name (e.g. a Biome asset named "Taiga" with a ScatterEntry
 * tagged "Tree_Pine" scatters pine trees wherever an imported row's BiomeName reads "Taiga") - the
 * "PCG/PCGEx graph will read these (matched by Tag)" hookup that struct's own comment already
 * anticipated, now with real per-point data to match them against.
 */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzPCGScatterPointRow : public FTableRowBase
{
	GENERATED_BODY()

	/** World-space position (UE units, cm) - the actual displaced surface point, already at Radius + Elevation, no further offset needed. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|PCG")
	FVector Position = FVector::ZeroVector;

	/** Outward surface normal (unit length) - for aligning a scattered prop to the local terrain slope rather than always straight up. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|PCG")
	FVector Normal = FVector::UpVector;

	/** Same convention as FSolarOrbzBiomeSampleContext::Elevation - height above/below the base radius, UE units (cm). Not yet adjusted for Sea Level; subtract your ClimateSimulation asset's own Sea Level (cm) if comparing against FSolarOrbzMaskRange::Elevation ranges authored relative to it. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|PCG")
	float ElevationCm = 0.0f;

	/** Same convention as FSolarOrbzBiomeSampleContext::Slope - 0 flat, 1 vertical. Averaged over this export's own (coarse) sample spacing, same documented trade-off as this project's other lat/long-grid-derived values - a sharp local cliff narrower than one sample cell reads as gentler than it really is; regenerate the standalone tool's export at a higher GridWidth/GridHeight if a biome's MinSlope/MaxSlope scatter rule needs finer resolution than this. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|PCG")
	float Slope = 0.0f;

	/** Same convention as FSolarOrbzBiomeSampleContext::Temperature - absolute Kelvin, not normalized. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|PCG")
	float TemperatureKelvin = 288.0f;

	/** Same convention as FSolarOrbzBiomeSampleContext::Moisture - 0 driest .. 1 wettest. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|PCG")
	float Moisture = 0.5f;

	/** The standalone tool's own classifier's biome name at this point (e.g. "Taiga", "Snow/Ice") - match against FSolarOrbzBiomeScatterEntry::Tag (or a Biome asset's own name) in your PCG graph's filter/branch logic. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|PCG")
	FName BiomeName;
};

/**
 * How USolarOrbzBiome::SelectMaterialIndex picks among Materials when a biome has more than one -
 * deliberately an enum rather than a bool, so a future selection rule (e.g. by elevation within
 * the biome, or deterministic random variation for texture-tiling breakup) is a new enum value
 * plus a new branch in SelectMaterialIndex, not a redesign. Only Slope is implemented so far -
 * requested that way directly, kept open for more later.
 */
UENUM(BlueprintType)
enum class ESolarOrbzBiomeMaterialSelector : uint8
{
	/**
	 * Materials[0] on the flattest ground this biome allows, Materials.Last() on the steepest,
	 * any entries in between spread evenly across [FlatSlope, SteepSlope]. Hard cutoff at each
	 * band boundary - no blend - since a baked static mesh triangle belongs to exactly one
	 * material slot (see ASolarOrbzIcoSphereActor::BakeToStaticMeshAsset); this is a discrete
	 * per-triangle choice, not a shader-side blend the way this plugin's old shared Biome Blend
	 * Material used to work.
	 */
	Slope,
};

UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzBiome : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Where this biome applies. Pick a mask type from the dropdown (Climate Mask, Composite Mask, ...) - this is where the actual condition lives. Leave unset for "always applies". */
	UPROPERTY(EditAnywhere, Instanced, Category = "SolarOrbz|Biome")
	TObjectPtr<USolarOrbzBiomeMask> Mask;

	/** Identifier color for this biome in editor visualizations (mask previews, debug views, etc). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	FLinearColor PreviewColor = FLinearColor::White;

	/**
	 * Extra terrain layers specific to this biome - e.g. a "Mountain" biome's own ruggedness noise.
	 * Blended in wherever this biome's mask is active, on top of the planet's base terrain stack.
	 *
	 * Kept fully functional for existing planets, but no longer the first reach for NEW biome-scoped
	 * terrain shaping: any USolarOrbzTerrainLayer now has its own optional Mask field, so the same
	 * effect (e.g. this biome's own noise, only where this biome applies) can be authored as a masked
	 * layer directly in the planet's main Terrain Layer Stack instead - with real blend-mode control
	 * and explicit ordering relative to every other layer, rather than being lumped into one additive
	 * pass after the whole base stack runs. A Composite Mask on that layer can reference this Biome
	 * asset directly to reuse its condition without redefining it. Still the right tool when a
	 * self-contained, always-additive biome-only detail pass genuinely is what you want.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TObjectPtr<USolarOrbzTerrainLayerStack> TerrainDetail;

	/**
	 * This biome's own ground material(s) - fed directly into the baked static mesh's material
	 * slots (see ASolarOrbzIcoSphereActor::BakeToStaticMeshAsset), replacing the old shared
	 * "one Texture2DArray + one blend material for the whole planet" system (BaseColorTexture/
	 * BiomeTextureArray/BuildBiomeTextureArray/the actor's own BiomeBlendMaterial - all removed).
	 * A single entry means "always use this one material, regardless of slope." With more than one
	 * entry, MaterialSelector decides which point on the mesh gets which - see
	 * SelectMaterialIndex().
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome|Material")
	TArray<TObjectPtr<class UMaterialInterface>> Materials;

	/** How to choose among Materials when there's more than one. Ignored (Materials[0] always used) when Materials has 0 or 1 entries. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome|Material", meta = (EditCondition = "Materials.Num() > 1"))
	ESolarOrbzBiomeMaterialSelector MaterialSelector = ESolarOrbzBiomeMaterialSelector::Slope;

	/**
	 * Slope selector only. At or below this slope, Materials[0] (the flattest-terrain entry) is
	 * used exclusively - same 0=flat/1=vertical-cliff convention as
	 * FSolarOrbzBiomeSampleContext::Slope, not degrees.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome|Material", meta = (EditCondition = "Materials.Num() > 1 && MaterialSelector == ESolarOrbzBiomeMaterialSelector::Slope", ClampMin = "0.0", ClampMax = "1.0"))
	float FlatSlope = 0.15f;

	/** Slope selector only. At or above this slope, Materials.Last() (the steepest-terrain entry) is used exclusively. Must be greater than Flat Slope, or every point collapses onto Materials[0]. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome|Material", meta = (EditCondition = "Materials.Num() > 1 && MaterialSelector == ESolarOrbzBiomeMaterialSelector::Slope", ClampMin = "0.0", ClampMax = "1.0"))
	float SteepSlope = 0.6f;

	/**
	 * Picks which entry of Materials applies at this point - a hard, discrete choice (no blend),
	 * since a baked static mesh triangle belongs to exactly one material slot. Returns INDEX_NONE
	 * if Materials is empty; always 0 if Materials has exactly one entry (MaterialSelector/
	 * FlatSlope/SteepSlope are only consulted once there are at least 2 to choose between).
	 */
	int32 SelectMaterialIndex(const FSolarOrbzBiomeSampleContext& Context) const;

	/** Placeholder for the future PCG/PCGEx hookup - what this biome scatters, and how densely. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TArray<FSolarOrbzBiomeScatterEntry> ScatterEntries;

	/**
	 * How strongly this biome applies at the given point, 0..1. Guards against a Composite Mask
	 * accidentally forming a reference cycle (Biome A's mask includes Biome B, whose mask includes
	 * Biome A) by capping recursion depth and logging a warning rather than hanging or crashing.
	 */
	float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const;
};

// ================================================================================================
// FSolarOrbzBiomeLayerEntry / USolarOrbzBiomeStack - an ordered list of (Biome, Opacity) layers,
// painted onto the planet Photoshop-style. Layers[0] is the bottom of the stack; the last entry is
// the topmost / highest priority. Where each biome applies comes from that Biome's own Mask now,
// not a separate reference here.
//
// SelectMaterial() resolves which real UMaterialInterface a given point should bake with: the
// single strongest-applying biome at that point (GetDominantBiome), asked which of its own
// Materials it wants there (USolarOrbzBiome::SelectMaterialIndex) - a hard, single-material answer
// per point, not a blend across several biomes at once (that's what EvaluateTopWeightedBiomes is
// for, kept for a future shader-blend use case, but SelectMaterial doesn't call it).
// ================================================================================================
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzBiomeLayerEntry
{
	GENERATED_BODY()

	/** Which biome this layer paints - where it applies comes from this Biome's own Mask. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|BiomeLayer")
	TObjectPtr<USolarOrbzBiome> Biome;

	/** Overall strength multiplier for this layer, independent of the mask. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|BiomeLayer", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Opacity = 1.0f;
};

UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzBiomeStack : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TArray<FSolarOrbzBiomeLayerEntry> Layers;

	/** Per-layer weight (mask weight * opacity) at this point, same order as Layers - useful for baking vertex colors or per-point PCG attributes later. Also the precomputed input GetDominantBiome/EvaluateBiomeTerrainContribution/EvaluateTopWeightedBiomes' overloads below expect, so a caller needing more than one of them at the same point only evaluates every layer's mask once. */
	void EvaluateLayerWeights(const FSolarOrbzBiomeSampleContext& Context, TArray<float>& OutWeights) const;

	/** The single strongest biome at this point, ties broken in favor of the topmost layer. Returns nullptr if no layer applies. */
	USolarOrbzBiome* GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context) const;

	/** Same result as above, but reuses LayerWeights (as produced by EvaluateLayerWeights for this same Context) instead of re-evaluating every layer's mask from scratch - use this when you're also calling EvaluateBiomeTerrainContribution/EvaluateTopWeightedBiomes at the same point, so each mask is only evaluated once. */
	USolarOrbzBiome* GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const;

	/** Extra terrain height from every biome's TerrainDetail stack, blended by that layer's weight. Call this after the base terrain stack has already displaced the vertex. */
	float EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context) const;

	/** Same as above, but reuses precomputed LayerWeights instead of re-evaluating every layer's mask - see GetDominantBiome's precomputed overload above. */
	float EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const;

	/** Every distinct Biome referenced anywhere in Layers, in first-appearance order. Duplicate references to the same Biome across multiple layer entries only appear once. */
	void GetUniqueBiomes(TArray<USolarOrbzBiome*>& OutBiomes) const;

	/**
	 * Finds up to MaxBiomes of the strongest-applying biomes at this point, sorted strongest-first,
	 * with weights renormalized to sum to 1 - for a future genuine multi-biome shader blend; nothing
	 * in this plugin calls this today (SelectMaterial always wants a single hard winner instead, via
	 * GetDominantBiome). OutBiomeIndices are indices into GetUniqueBiomes()'s order, not into Layers.
	 * Arrays returned shorter than MaxBiomes when fewer than MaxBiomes biomes have any weight here at all.
	 */
	void EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const;

	/** Same as above, but reuses precomputed LayerWeights and a precomputed UniqueBiomes list (as produced by GetUniqueBiomes - invariant for the whole regenerate, so callers evaluating this per-vertex should compute it once beforehand rather than every call) instead of rebuilding/re-evaluating either from scratch. */
	void EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights, const TArray<USolarOrbzBiome*>& UniqueBiomes, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const;

	/**
	 * The material the dominant biome at this point wants to use (that biome's own
	 * SelectMaterialIndex, applied to its Materials array) - nullptr if no biome's Mask applies
	 * here at all, or the dominant biome has an empty Materials array. A hard, single-material
	 * answer for one point, not a blend - see USolarOrbzBiome::Materials' own comment for why.
	 */
	class UMaterialInterface* SelectMaterial(const FSolarOrbzBiomeSampleContext& Context) const;

	/** Same as above, but reuses precomputed LayerWeights instead of re-evaluating every layer's mask - see GetDominantBiome's precomputed overload above. */
	class UMaterialInterface* SelectMaterial(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const;

	/**
	 * Writes every Biome in this stack's ScatterEntries (that have a ScatterMesh assigned - entries
	 * without one are skipped, each logged once as a warning) as a real PCGEx Mesh Collection import
	 * document: "format": "pcgex.object", "version": 1, "data": { "class":
	 * "/Script/PCGExCollections.PCGExMeshCollection", "members": { "Entries": [...] } }, one entry per
	 * ScatterEntry with "type": "Mesh", "StaticMesh": <ScatterMesh's path>, "Category": <Tag>, "Weight":
	 * 1. This schema is matched against PCGEx's own documented agent-authoring format (pcgex.gitbook.io/
	 * pcgex/developers-area/authoring-with-an-agent/collections, fetched directly - a real worked
	 * example there is a one-entry Mesh Collection using this exact shape) and against
	 * FPCGExAssetCollectionEntry's own real UPROPERTY names (Weight, Category - read directly from
	 * PCGEx/PCGExtendedToolkit's own public source, not guessed), NOT compiled or run against PCGEx
	 * itself - this plugin has no dependency on PCGEx and doesn't need one for this export, since it's
	 * plain JSON text generation, matching PCGEx's own documented import format, exactly the way
	 * NoiseBasis.h's standalone-tool port doesn't need UE itself to produce comparable output. Feed the
	 * written file to PCGEx's own ImportJson/CreateAsset tool actions (via the PCGEx Assistant plugin)
	 * to get a real, editable Mesh Collection asset.
	 *
	 * DensityPerSquareMeter/MinSlope/MaxSlope are NOT part of this document - PCGEx itself separates
	 * "what to place" (a collection entry, exported here) from "how densely, where" (a separate
	 * Distribute-family node's own settings, a different document this function doesn't have a
	 * verified schema for) - so those three are logged instead (one line per exported entry), for
	 * hand-entry into whichever placement/distribution node a PCGEx graph actually uses.
	 *
	 * Writes to Saved/SolarOrbzPCGExExports/<this asset's name>_ScatterCollection.json (same
	 * ProjectSavedDir()-relative convention as GetTectonicBackupFilePath), creating the directory if
	 * needed. No-op (logged) if no ScatterEntry anywhere in this stack has a ScatterMesh assigned.
	 */
	UFUNCTION(CallInEditor, Category = "SolarOrbz|Biome|PCGEx")
	void ExportScatterEntriesToPCGExMeshCollectionJson();
};
