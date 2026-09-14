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

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzBiomeSystem.generated.h"

class USolarOrbzTerrainLayerStack;
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

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
	virtual bool NeedsClimateData() const override { return Temperature.bEnabled || Moisture.bEnabled; }
	virtual bool NeedsSlope() const override { return Slope.bEnabled; }
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
};

// ================================================================================================
// FSolarOrbzBiomeScatterEntry / USolarOrbzBiome - a reusable bundle: the mask deciding where it
// applies, optional extra terrain detail (blended in wherever that mask is active), and scatter
// definitions that will feed a future PCG/PCGEx hookup. Store these as assets and reuse them
// across planets, the same way World Creator's biome presets work.
// ================================================================================================

/**
 * Placeholder scatter rule - enough to describe "what goes where and how densely" for now.
 * The PCG/PCGEx graph will read these (matched by Tag) once that hookup exists.
 */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzBiomeScatterEntry
{
	GENERATED_BODY()

	/** Matched against PCG graph settings later - e.g. "Rock_Large", "Tree_Pine", "Wildlife_Deer". */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0"))
	float DensityPerSquareMeter = 0.01f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSlope = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxSlope = 1.0f;
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
	 * This biome's ground texture. Fed into the owning Biome Stack's texture array (see
	 * USolarOrbzBiomeStack::BuildBiomeTextureArray) rather than assigned to a material directly -
	 * a single shared material blends up to 4 of these per point based on which biomes actually
	 * apply there, so any number of biomes can exist on one planet even though only ~4 ever need to
	 * render at any single point.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TObjectPtr<class UTexture2D> BaseColorTexture;

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
// The stack also owns the texture-array bridge for material rendering: any number of biomes can
// exist here, but a single point on the mesh only ever blends the top ~4 strongest-applying ones
// (see EvaluateTopWeightedBiomes) - so ground texturing scales to a full biome palette without
// needing a material with one texture slot per biome.
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

	/**
	 * Built by BuildBiomeTextureArray() below from every unique biome's Base Color Texture, in the
	 * same order GetUniqueBiomes() returns - that order IS the array index each biome is addressed
	 * by from a vertex's encoded indices. Assign a material with a Texture2DArray parameter named
	 * "BiomeTextureArray" to the actor's Biome Blend Material to read this.
	 */
	UPROPERTY(VisibleAnywhere, Category = "SolarOrbz|Biome|Material")
	TObjectPtr<class UTexture2DArray> BiomeTextureArray;

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

	/** Every distinct Biome referenced anywhere in Layers, in first-appearance order - this order defines each biome's texture array index. Duplicate references to the same Biome across multiple layer entries only appear once. */
	void GetUniqueBiomes(TArray<USolarOrbzBiome*>& OutBiomes) const;

	/**
	 * Finds up to MaxBiomes of the strongest-applying biomes at this point, sorted strongest-first,
	 * with weights renormalized to sum to 1 (ready to feed straight into a weighted material blend).
	 * OutBiomeIndices are indices into GetUniqueBiomes()'s order, not into Layers - i.e. exactly the
	 * indices BiomeTextureArray's slices are addressed by. Arrays returned shorter than MaxBiomes
	 * when fewer than MaxBiomes biomes have any weight here at all.
	 */
	void EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const;

	/** Same as above, but reuses precomputed LayerWeights and a precomputed UniqueBiomes list (as produced by GetUniqueBiomes - invariant for the whole regenerate, so callers evaluating this per-vertex should compute it once beforehand rather than every call) instead of rebuilding/re-evaluating either from scratch. */
	void EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights, const TArray<USolarOrbzBiome*>& UniqueBiomes, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const;

	/**
	 * Rebuilds BiomeTextureArray from every unique biome's Base Color Texture, in GetUniqueBiomes()
	 * order. Editor-only. Call after adding/removing/reordering biomes in Layers, or changing a
	 * biome's texture - the actor calls this automatically during Regenerate Mesh if the array looks
	 * stale (wrong biome count), so you don't strictly have to remember it, but it's exposed here too
	 * for an explicit rebuild (e.g. from a content pipeline script).
	 */
	UFUNCTION(CallInEditor, Category = "SolarOrbz|Biome|Material")
	void BuildBiomeTextureArray();
};
