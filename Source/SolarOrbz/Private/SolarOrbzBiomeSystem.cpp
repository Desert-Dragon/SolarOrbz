// SolarOrbz - Biome System subsystem implementation.

#include "SolarOrbzBiomeSystem.h"
#include "SolarOrbzTerrainLayers.h"

// ================================================================================================
// USolarOrbzBiomeMask - GetWeight has an inline default; nothing else to implement here.
// ================================================================================================

// ================================================================================================
// FSolarOrbzMaskRange / USolarOrbzClimateBiomeMask
// ================================================================================================
float FSolarOrbzMaskRange::Evaluate(float Value) const
{
	if (!bEnabled)
	{
		return 1.0f;
	}

	if (Falloff <= KINDA_SMALL_NUMBER)
	{
		return (Value >= Min && Value <= Max) ? 1.0f : 0.0f;
	}

	const float LowEdge = FMath::SmoothStep(Min - Falloff, Min + Falloff, Value);
	const float HighEdge = 1.0f - FMath::SmoothStep(Max - Falloff, Max + Falloff, Value);
	return FMath::Clamp(FMath::Min(LowEdge, HighEdge), 0.0f, 1.0f);
}

float USolarOrbzClimateBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	float Weight = 1.0f;

	Weight *= Elevation.Evaluate((Context.Elevation / 100.0f) - Context.SeaLevel); // cm -> meters, then offset so the range tracks Sea Level rather than the raw base radius
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Latitude.Evaluate(FMath::Abs(Context.UnitDirection.Z));
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Slope.Evaluate(Context.Slope);
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	if (Temperature.bEnabled)
	{
		// Simulated (Kelvin) when available; otherwise a generic Earth-like Lerp by latitude, in the
		// same Kelvin units, so the Min/Max above stay meaningful whether or not a simulation exists.
		const float TemperatureValue = Context.bHasClimateData
			? Context.Temperature
			: FMath::Lerp(288.0f, 255.0f, FMath::Abs(Context.UnitDirection.Z));
		Weight *= Temperature.Evaluate(TemperatureValue);
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}
	}

	if (Moisture.bEnabled)
	{
		float MoistureValue;
		if (Context.bHasClimateData)
		{
			MoistureValue = Context.Moisture;
		}
		else
		{
			const FVector SeedOffset(
				FMath::Frac(FMath::Sin((float)MoistureSeed * 91.345f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 13.71f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 58.92f) * 47453.7f) * 1000.0f);

			MoistureValue = FMath::PerlinNoise3D(Context.UnitDirection * MoistureFrequency + SeedOffset) * 0.5f + 0.5f; // 0..1
		}
		Weight *= Moisture.Evaluate(MoistureValue);
	}

	return FMath::Clamp(Weight, 0.0f, 1.0f);
}

// ================================================================================================
// USolarOrbzBiomeMaskPreset - GetWeight is inline; nothing else to implement here.
// ================================================================================================

// ================================================================================================
// USolarOrbzCompositeBiomeMask
// ================================================================================================
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

// ================================================================================================
// USolarOrbzBiome - no logic, just data; nothing to implement here.
// ================================================================================================

// ================================================================================================
// USolarOrbzBiomeStack
// ================================================================================================
void USolarOrbzBiomeStack::EvaluateLayerWeights(const FSolarOrbzBiomeSampleContext& Context, TArray<float>& OutWeights) const
{
	OutWeights.Reset(Layers.Num());
	for (const FSolarOrbzBiomeLayerEntry& Entry : Layers)
	{
		const float MaskWeight = Entry.Mask ? Entry.Mask->GetWeight(Context) : 1.0f;
		OutWeights.Add(FMath::Clamp(MaskWeight * Entry.Opacity, 0.0f, 1.0f));
	}
}

USolarOrbzBiome* USolarOrbzBiomeStack::GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context) const
{
	USolarOrbzBiome* Best = nullptr;
	float BestWeight = 0.0f;

	// Iterate forward and use >= so later (topmost) entries win ties, matching the Photoshop-style ordering.
	for (const FSolarOrbzBiomeLayerEntry& Entry : Layers)
	{
		if (!Entry.Biome)
		{
			continue;
		}

		const float MaskWeight = (Entry.Mask ? Entry.Mask->GetWeight(Context) : 1.0f) * Entry.Opacity;
		if (MaskWeight >= BestWeight)
		{
			BestWeight = MaskWeight;
			Best = Entry.Biome;
		}
	}

	return BestWeight > KINDA_SMALL_NUMBER ? Best : nullptr;
}

float USolarOrbzBiomeStack::EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context) const
{
	float Accum = 0.0f;

	for (const FSolarOrbzBiomeLayerEntry& Entry : Layers)
	{
		if (!Entry.Biome || !Entry.Biome->TerrainDetail)
		{
			continue;
		}

		const float Weight = (Entry.Mask ? Entry.Mask->GetWeight(Context) : 1.0f) * Entry.Opacity;
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const float DetailHeight = Entry.Biome->TerrainDetail->EvaluateHeight(Context.UnitDirection, Context.UV);
		Accum += DetailHeight * Weight;
	}

	return Accum;
}
