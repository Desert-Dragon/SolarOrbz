// SolarOrbz - Composite biome mask implementation.

#include "SolarOrbzCompositeBiomeMask.h"
#include "SolarOrbzBiomeMaskPreset.h"

float USolarOrbzCompositeBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (Masks.Num() == 0)
	{
		return 1.0f; // no children = no restriction, matching an "empty AND is true" convention
	}

	switch (CombineMode)
	{
	case ESolarOrbzMaskCombineMode::Multiply:
	{
		float Result = 1.0f;
		for (const TObjectPtr<USolarOrbzBiomeMaskPreset>& MaskPreset : Masks)
		{
			if (MaskPreset)
			{
				Result *= MaskPreset->GetWeight(Context);
				if (Result <= KINDA_SMALL_NUMBER)
				{
					return 0.0f;
				}
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Min:
	{
		float Result = 1.0f;
		for (const TObjectPtr<USolarOrbzBiomeMaskPreset>& MaskPreset : Masks)
		{
			if (MaskPreset)
			{
				Result = FMath::Min(Result, MaskPreset->GetWeight(Context));
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Max:
	{
		float Result = 0.0f;
		for (const TObjectPtr<USolarOrbzBiomeMaskPreset>& MaskPreset : Masks)
		{
			if (MaskPreset)
			{
				Result = FMath::Max(Result, MaskPreset->GetWeight(Context));
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Average:
	{
		float Sum = 0.0f;
		int32 Count = 0;
		for (const TObjectPtr<USolarOrbzBiomeMaskPreset>& MaskPreset : Masks)
		{
			if (MaskPreset)
			{
				Sum += MaskPreset->GetWeight(Context);
				++Count;
			}
		}
		return Count > 0 ? (Sum / Count) : 1.0f;
	}
	}

	return 1.0f;
}
