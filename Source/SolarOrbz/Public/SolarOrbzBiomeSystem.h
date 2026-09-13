// SolarOrbz - Biome System subsystem. Combines the mask base class/sample context, the Climate
// and Composite mask types, the standalone Mask Preset asset, the Biome asset, and the ordered
// Biome Stack into one file. Grouped together because they form one tightly-coupled system: masks
// answer "how strongly does a biome apply here" (0..1) given a point's climate/shape inputs, biomes
// bundle what to actually paint when a mask matches, and the stack combines many (Biome, Mask)
// pairs Photoshop-style across a whole planet.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzBiomeSystem.generated.h"

class USolarOrbzTerrainLayerStack;

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
};

// ================================================================================================
// USolarOrbzBiomeMaskPreset - a reusable, standalone asset holding one mask (Climate, Composite,
// etc). Create these directly in the Content Browser (right-click -> Miscellaneous -> Data Asset
// -> SolarOrbzBiomeMaskPreset), then reference the same preset from as many biome layers or
// composite masks as you like - e.g. author "Near Poles" once, reuse it everywhere.
// ================================================================================================
UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzBiomeMaskPreset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Pick a mask type from the dropdown (Climate Mask, Composite Mask, ...) - this is where the actual condition lives. */
	UPROPERTY(EditAnywhere, Instanced, Category = "SolarOrbz|Mask")
	TObjectPtr<USolarOrbzBiomeMask> RootMask;

	float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
	{
		return RootMask ? RootMask->GetWeight(Context) : 1.0f;
	}
};

// ================================================================================================
// USolarOrbzCompositeBiomeMask - combines other mask presets so compound biome variants ("Icy
// Chemical" + "Mountain") are built by stacking simple, reusable masks rather than needing a
// dedicated class or enum entry per combination. Children are Mask Preset asset references (not
// embedded masks) so the same building-block mask (e.g. "Near Poles") can be reused across many
// composites.
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
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Composite")
	TArray<TObjectPtr<USolarOrbzBiomeMaskPreset>> Masks;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Composite")
	ESolarOrbzMaskCombineMode CombineMode = ESolarOrbzMaskCombineMode::Multiply;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
};

// ================================================================================================
// FSolarOrbzBiomeScatterEntry / USolarOrbzBiome - a reusable bundle: optional extra terrain detail
// (blended in wherever this biome's mask is active) and scatter definitions that will feed a
// future PCG/PCGEx hookup. Store these as assets and reuse them across planets, the same way World
// Creator's biome presets work.
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
	/** Identifier color for this biome in editor visualizations (mask previews, debug views, etc). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	FLinearColor PreviewColor = FLinearColor::White;

	/** Extra terrain layers specific to this biome - e.g. a "Mountain" biome's own ruggedness noise. Blended in wherever this biome's mask is active, on top of the planet's base terrain stack. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TObjectPtr<USolarOrbzTerrainLayerStack> TerrainDetail;

	/** Placeholder for the future PCG/PCGEx hookup - what this biome scatters, and how densely. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TArray<FSolarOrbzBiomeScatterEntry> ScatterEntries;
};

// ================================================================================================
// FSolarOrbzBiomeLayerEntry / USolarOrbzBiomeStack - an ordered list of (Biome, MaskPreset,
// Opacity) layers, painted onto the planet Photoshop-style. Layers[0] is the bottom of the stack;
// the last entry is the topmost / highest priority.
// ================================================================================================
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzBiomeLayerEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|BiomeLayer")
	TObjectPtr<USolarOrbzBiome> Biome;

	/** Where this biome applies. Reference a Mask Preset asset - create one via right-click -> Miscellaneous -> Data Asset -> SolarOrbzBiomeMaskPreset. Leave unset for "always applies". */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|BiomeLayer")
	TObjectPtr<USolarOrbzBiomeMaskPreset> Mask;

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

	/** Per-layer weight (mask weight * opacity) at this point, same order as Layers - useful for baking vertex colors or per-point PCG attributes later. */
	void EvaluateLayerWeights(const FSolarOrbzBiomeSampleContext& Context, TArray<float>& OutWeights) const;

	/** The single strongest biome at this point, ties broken in favor of the topmost layer. Returns nullptr if no layer applies. */
	USolarOrbzBiome* GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context) const;

	/** Extra terrain height from every biome's TerrainDetail stack, blended by that layer's weight. Call this after the base terrain stack has already displaced the vertex. */
	float EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context) const;
};
