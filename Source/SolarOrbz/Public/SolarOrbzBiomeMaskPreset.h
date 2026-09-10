// SolarOrbz - Mask preset. A reusable, standalone asset holding one mask
// (Climate, Composite, etc). Create these directly in the Content Browser
// (right-click -> Miscellaneous -> Data Asset -> SolarOrbzBiomeMaskPreset),
// then reference the same preset from as many biome layers or composite
// masks as you like - e.g. author "Near Poles" once, reuse it everywhere.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzBiomeMask.h"
#include "SolarOrbzBiomeMaskPreset.generated.h"

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
