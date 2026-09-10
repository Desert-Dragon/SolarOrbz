// SolarOrbz - Composite biome mask. Combines other mask presets so compound
// biome variants ("Icy Chemical" + "Mountain") are built by stacking simple,
// reusable masks rather than needing a dedicated class or enum entry per
// combination. Children are Mask Preset asset references (not embedded
// masks) so the same building-block mask (e.g. "Near Poles") can be reused
// across many composites.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzBiomeMask.h"
#include "SolarOrbzCompositeBiomeMask.generated.h"

class USolarOrbzBiomeMaskPreset;

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
