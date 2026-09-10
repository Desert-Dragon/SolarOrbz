// SolarOrbz - Biome stack: an ordered list of (Biome, MaskPreset, Opacity)
// layers, painted onto the planet Photoshop-style. Layers[0] is the bottom
// of the stack; the last entry is the topmost / highest priority.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzBiomeMask.h"
#include "SolarOrbzBiomeStack.generated.h"

class USolarOrbzBiome;
class USolarOrbzBiomeMaskPreset;

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
