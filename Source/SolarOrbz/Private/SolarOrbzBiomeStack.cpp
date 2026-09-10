// SolarOrbz - Biome stack implementation.

#include "SolarOrbzBiomeStack.h"
#include "SolarOrbzBiome.h"
#include "SolarOrbzBiomeMaskPreset.h"
#include "SolarOrbzTerrainLayerStack.h"

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
